/*
 * event_loop.c - epoll-based event loop with TCP listener support.
 * Single-threaded, non-blocking I/O using edge-triggered epoll.
 */

#define _GNU_SOURCE

#include "event_loop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define INITIAL_BUF_CAP  4096
#define MAX_EVENTS       64
#define DEFAULT_MAX_CONNECTIONS 1024

// ---- Internal helpers ----

static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

static void buffer_init(Buffer* buf) {
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static bool buffer_ensure(Buffer* buf, size_t additional) {
    size_t needed = buf->len + additional;
    if (needed <= buf->cap) return true;

    size_t new_cap = buf->cap < INITIAL_BUF_CAP ? INITIAL_BUF_CAP : buf->cap;
    while (new_cap < needed) {
        new_cap *= 2;
    }

    uint8_t* new_data = realloc(buf->data, new_cap);
    if (!new_data) return false;

    buf->data = new_data;
    buf->cap = new_cap;
    return true;
}

static void buffer_free(Buffer* buf) {
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

// Tag types for epoll user data to distinguish listeners, connections, timers
#define TAG_LISTENER   1
#define TAG_CONNECTION 2
#define TAG_TIMER      3

typedef struct {
    int tag;
    void* ptr;
} EpollData;

// We store EpollData* in epoll_event.data.ptr
// These are allocated on the heap and freed when the fd is removed

static EpollData* epoll_data_new(int tag, void* ptr) {
    EpollData* ed = malloc(sizeof(EpollData));
    if (!ed) return NULL;
    ed->tag = tag;
    ed->ptr = ptr;
    return ed;
}

// ---- Event Loop Lifecycle ----

bool event_loop_init(EventLoop* loop) {
    loop->epoll_fd = epoll_create1(0);
    if (loop->epoll_fd == -1) return false;

    loop->running = false;
    loop->conn_head = NULL;
    loop->conn_count = 0;
    loop->max_connections = DEFAULT_MAX_CONNECTIONS;
    loop->listener_head = NULL;
    loop->timer_head = NULL;
    return true;
}

void event_loop_destroy(EventLoop* loop) {
    // Close all connections
    Connection* conn = loop->conn_head;
    while (conn) {
        Connection* next = conn->next;
        if (conn->fd >= 0) close(conn->fd);
        buffer_free(&conn->read_buf);
        buffer_free(&conn->write_buf);
        free(conn->epoll_data);
        free(conn);
        conn = next;
    }
    loop->conn_head = NULL;
    loop->conn_count = 0;

    // Close all listeners
    Listener* lis = loop->listener_head;
    while (lis) {
        Listener* next = lis->next;
        if (lis->fd >= 0) close(lis->fd);
        free(lis->epoll_data);
        free(lis);
        lis = next;
    }
    loop->listener_head = NULL;

    // Close all timers
    Timer* timer = loop->timer_head;
    while (timer) {
        Timer* next = timer->next;
        if (timer->fd >= 0) close(timer->fd);
        free(timer);
        timer = next;
    }
    loop->timer_head = NULL;

    // Close epoll fd
    if (loop->epoll_fd >= 0) {
        close(loop->epoll_fd);
        loop->epoll_fd = -1;
    }
}

// ---- Listener ----

bool event_loop_listen(EventLoop* loop, int port,
                       AcceptCallback on_accept, void* userdata) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) return false;

    // Set SO_REUSEADDR and SO_REUSEPORT
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    if (!set_nonblocking(sockfd)) {
        close(sockfd);
        return false;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        close(sockfd);
        return false;
    }

    if (listen(sockfd, 128) == -1) {
        close(sockfd);
        return false;
    }

    // Create listener struct
    Listener* lis = malloc(sizeof(Listener));
    if (!lis) {
        close(sockfd);
        return false;
    }
    lis->fd = sockfd;
    lis->on_accept = on_accept;
    lis->userdata = userdata;
    lis->epoll_data = NULL;
    lis->next = loop->listener_head;
    loop->listener_head = lis;

    // Register with epoll (edge-triggered for accept)
    EpollData* ed = epoll_data_new(TAG_LISTENER, lis);
    if (!ed) {
        close(sockfd);
        free(lis);
        return false;
    }
    lis->epoll_data = ed;

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = ed;

    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, sockfd, &ev) == -1) {
        close(sockfd);
        free(ed);
        free(lis);
        return false;
    }

    return true;
}

// ---- Connection Management ----

static Connection* connection_new(int fd) {
    Connection* conn = malloc(sizeof(Connection));
    if (!conn) return NULL;

    conn->fd = fd;
    conn->state = CONN_READING;
    buffer_init(&conn->read_buf);
    buffer_init(&conn->write_buf);
    conn->write_offset = 0;
    conn->userdata = NULL;
    conn->epoll_data = NULL;
    conn->on_data = NULL;
    conn->on_write_done = NULL;
    conn->next = NULL;
    conn->prev = NULL;
    return conn;
}

static void connection_link(EventLoop* loop, Connection* conn) {
    conn->next = loop->conn_head;
    conn->prev = NULL;
    if (loop->conn_head) {
        loop->conn_head->prev = conn;
    }
    loop->conn_head = conn;
    loop->conn_count++;
}

static void connection_unlink(EventLoop* loop, Connection* conn) {
    if (conn->prev) {
        conn->prev->next = conn->next;
    } else {
        loop->conn_head = conn->next;
    }
    if (conn->next) {
        conn->next->prev = conn->prev;
    }
    loop->conn_count--;
}

// ---- Accept handling ----

static void handle_accept(EventLoop* loop, Listener* lis) {
    // Edge-triggered: must accept all pending connections
    for (;;) {
        // Check connection limit before accepting
        if (loop->max_connections > 0 && loop->conn_count >= loop->max_connections) {
            // At capacity - accept and immediately close to drain the queue
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int client_fd = accept(lis->fd, (struct sockaddr*)&client_addr, &addr_len);
            if (client_fd >= 0) {
                close(client_fd);
            }
            break;
        }

        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(lis->fd, (struct sockaddr*)&client_addr, &addr_len);

        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // No more pending connections
            }
            if (errno == EINTR) continue;
            if (errno == EMFILE || errno == ENFILE) {
                // File descriptor exhaustion - stop accepting for now
                break;
            }
            break;  // Other error, stop accepting
        }

        if (!set_nonblocking(client_fd)) {
            close(client_fd);
            continue;
        }

        Connection* conn = connection_new(client_fd);
        if (!conn) {
            close(client_fd);
            continue;
        }

        connection_link(loop, conn);

        // Register with epoll for reading (edge-triggered)
        // Store EpollData on the connection for reuse across MODs
        EpollData* ed = epoll_data_new(TAG_CONNECTION, conn);
        if (!ed) {
            event_loop_conn_close(loop, conn);
            continue;
        }
        conn->epoll_data = ed;

        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.ptr = ed;

        if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
            conn->epoll_data = NULL;
            free(ed);
            event_loop_conn_close(loop, conn);
            continue;
        }

        // Call the accept callback
        if (lis->on_accept) {
            lis->on_accept(loop, conn, lis->userdata);
        }
    }
}

// ---- Read handling ----

static void handle_read(EventLoop* loop, Connection* conn, EpollData* ed) {
    if (conn->state == CONN_CLOSED) return;

    // Edge-triggered: must read all available data
    for (;;) {
        if (!buffer_ensure(&conn->read_buf, 4096)) {
            event_loop_conn_close(loop, conn);
            return;
        }

        ssize_t n = read(conn->fd, conn->read_buf.data + conn->read_buf.len,
                         conn->read_buf.cap - conn->read_buf.len);

        if (n > 0) {
            conn->read_buf.len += (size_t)n;
        } else if (n == 0) {
            // Connection closed by peer
            conn->state = CONN_CLOSED;
            if (conn->on_data) {
                conn->on_data(loop, conn, conn->userdata);
            }
            event_loop_conn_close(loop, conn);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // No more data available
            }
            if (errno == EINTR) continue;
            // Read error
            event_loop_conn_close(loop, conn);
            return;
        }
    }

    // Call data callback
    if (conn->on_data && conn->read_buf.len > 0) {
        conn->on_data(loop, conn, conn->userdata);
    }

    (void)ed;
}

// ---- Write handling ----

static void handle_write(EventLoop* loop, Connection* conn, EpollData* ed) {
    if (conn->state == CONN_CLOSED) return;

    // Write as much as possible
    while (conn->write_offset < conn->write_buf.len) {
        size_t remaining = conn->write_buf.len - conn->write_offset;
        ssize_t n = write(conn->fd,
                          conn->write_buf.data + conn->write_offset,
                          remaining);

        if (n > 0) {
            conn->write_offset += (size_t)n;
        } else if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // Cannot write more right now
            }
            if (errno == EINTR) continue;
            // Write error
            event_loop_conn_close(loop, conn);
            return;
        }
    }

    // Check if all data has been written
    if (conn->write_offset >= conn->write_buf.len) {
        // Reset write buffer
        conn->write_buf.len = 0;
        conn->write_offset = 0;
        conn->state = CONN_READING;

        // Remove EPOLLOUT interest, keep EPOLLIN
        // Reuse the EpollData stored on the connection
        EpollData* conn_ed = (EpollData*)conn->epoll_data;
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.ptr = conn_ed ? conn_ed : ed;
        epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);

        // Call write-done callback
        if (conn->on_write_done) {
            conn->on_write_done(loop, conn, conn->userdata);
        }
    }

    (void)ed;
}

// ---- Timer handling ----

static void handle_timer(EventLoop* loop, Timer* timer) {
    // Read the timerfd to acknowledge it
    uint64_t expirations;
    ssize_t n = read(timer->fd, &expirations, sizeof(expirations));
    (void)n;

    // Call the timer callback
    if (timer->callback) {
        timer->callback(loop, timer->userdata);
    }

    // One-shot: remove and free the timer
    event_loop_cancel_timer(loop, timer);
}

// ---- Public API: conn_write ----

bool event_loop_conn_write(EventLoop* loop, Connection* conn,
                           const void* data, size_t len) {
    if (conn->state == CONN_CLOSED) return false;
    if (len == 0) return true;

    if (!buffer_ensure(&conn->write_buf, len)) return false;

    memcpy(conn->write_buf.data + conn->write_buf.len, data, len);
    conn->write_buf.len += len;
    conn->state = CONN_WRITING;

    // Try to write immediately
    while (conn->write_offset < conn->write_buf.len) {
        size_t remaining = conn->write_buf.len - conn->write_offset;
        ssize_t n = write(conn->fd,
                          conn->write_buf.data + conn->write_offset,
                          remaining);
        if (n > 0) {
            conn->write_offset += (size_t)n;
        } else if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // Need to wait for EPOLLOUT
            }
            if (errno == EINTR) continue;
            return false;  // Write error
        }
    }

    // If all written, reset and stay in READING state
    if (conn->write_offset >= conn->write_buf.len) {
        conn->write_buf.len = 0;
        conn->write_offset = 0;
        conn->state = CONN_READING;
        if (conn->on_write_done) {
            conn->on_write_done(loop, conn, conn->userdata);
        }
        return true;
    }

    // Need to register EPOLLOUT to continue writing later
    // Reuse the EpollData already stored on the connection
    EpollData* ed = (EpollData*)conn->epoll_data;
    if (!ed) {
        // Shouldn't happen, but handle gracefully
        ed = epoll_data_new(TAG_CONNECTION, conn);
        if (!ed) return false;
        conn->epoll_data = ed;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.ptr = ed;

    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev) == -1) {
        return false;
    }

    return true;
}

// ---- Public API: conn_close ----

void event_loop_conn_close(EventLoop* loop, Connection* conn) {
    if (conn->state == CONN_CLOSED && conn->fd < 0) return;

    // Remove from epoll (automatically done on close, but be explicit)
    if (conn->fd >= 0) {
        epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
        close(conn->fd);
        conn->fd = -1;
    }

    // Free the owned EpollData
    if (conn->epoll_data) {
        free(conn->epoll_data);
        conn->epoll_data = NULL;
    }

    conn->state = CONN_CLOSED;
    buffer_free(&conn->read_buf);
    buffer_free(&conn->write_buf);

    connection_unlink(loop, conn);
    free(conn);
}

// ---- Public API: timers ----

Timer* event_loop_add_timer(EventLoop* loop, uint64_t ms,
                            TimerCallback callback, void* userdata) {
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (tfd == -1) return NULL;

    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = (time_t)(ms / 1000);
    its.it_value.tv_nsec = (long)((ms % 1000) * 1000000);

    if (timerfd_settime(tfd, 0, &its, NULL) == -1) {
        close(tfd);
        return NULL;
    }

    Timer* timer = malloc(sizeof(Timer));
    if (!timer) {
        close(tfd);
        return NULL;
    }

    timer->fd = tfd;
    timer->callback = callback;
    timer->userdata = userdata;
    timer->next = loop->timer_head;
    loop->timer_head = timer;

    // Register with epoll
    EpollData* ed = epoll_data_new(TAG_TIMER, timer);
    if (!ed) {
        close(tfd);
        free(timer);
        return NULL;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = ed;

    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, tfd, &ev) == -1) {
        close(tfd);
        free(ed);
        free(timer);
        return NULL;
    }

    return timer;
}

void event_loop_cancel_timer(EventLoop* loop, Timer* timer) {
    if (!timer) return;

    // Remove from epoll
    if (timer->fd >= 0) {
        epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, timer->fd, NULL);
        close(timer->fd);
        timer->fd = -1;
    }

    // Remove from list
    Timer** pp = &loop->timer_head;
    while (*pp) {
        if (*pp == timer) {
            *pp = timer->next;
            break;
        }
        pp = &(*pp)->next;
    }

    free(timer);
}

// ---- Public API: run / stop ----

void event_loop_run(EventLoop* loop) {
    loop->running = true;
    struct epoll_event events[MAX_EVENTS];

    while (loop->running) {
        int nfds = epoll_wait(loop->epoll_fd, events, MAX_EVENTS, 100);

        if (nfds == -1) {
            if (errno == EINTR) continue;
            break;  // Fatal epoll error
        }

        for (int i = 0; i < nfds; i++) {
            EpollData* ed = (EpollData*)events[i].data.ptr;
            if (!ed) continue;

            uint32_t ev_flags = events[i].events;

            switch (ed->tag) {
                case TAG_LISTENER:
                    if (ev_flags & EPOLLIN) {
                        handle_accept(loop, (Listener*)ed->ptr);
                    }
                    break;

                case TAG_CONNECTION: {
                    Connection* conn = (Connection*)ed->ptr;
                    if (ev_flags & (EPOLLERR | EPOLLHUP)) {
                        // EpollData is freed inside event_loop_conn_close
                        event_loop_conn_close(loop, conn);
                        break;
                    }
                    if (ev_flags & EPOLLIN) {
                        handle_read(loop, conn, ed);
                    }
                    if (ev_flags & EPOLLOUT) {
                        // Check conn is still valid (might have been closed in read handler)
                        if (conn->state != CONN_CLOSED) {
                            handle_write(loop, conn, ed);
                        }
                    }
                    break;
                }

                case TAG_TIMER:
                    if (ev_flags & EPOLLIN) {
                        handle_timer(loop, (Timer*)ed->ptr);
                        free(ed);
                    }
                    break;
            }
        }
    }
}

void event_loop_stop(EventLoop* loop) {
    loop->running = false;
}
