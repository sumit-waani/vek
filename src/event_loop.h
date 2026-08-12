#ifndef VEK_EVENT_LOOP_H
#define VEK_EVENT_LOOP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Forward declarations
typedef struct Connection Connection;
typedef struct EventLoop EventLoop;
typedef struct Timer Timer;

// Connection states
typedef enum {
    CONN_READING,
    CONN_WRITING,
    CONN_CLOSED
} ConnState;

// Callback types
typedef void (*AcceptCallback)(EventLoop* loop, Connection* conn, void* userdata);
typedef void (*DataCallback)(EventLoop* loop, Connection* conn, void* userdata);
typedef void (*WriteDoneCallback)(EventLoop* loop, Connection* conn, void* userdata);
typedef void (*TimerCallback)(EventLoop* loop, void* userdata);

// Growable buffer
typedef struct {
    uint8_t* data;
    size_t   len;
    size_t   cap;
} Buffer;

// Connection structure
struct Connection {
    int         fd;
    ConnState   state;
    Buffer      read_buf;
    Buffer      write_buf;
    size_t      write_offset;  // How much of write_buf has been sent
    void*       userdata;
    void*       epoll_data;    // Owned EpollData pointer (reused across MODs)

    // Callbacks
    DataCallback      on_data;
    WriteDoneCallback on_write_done;

    // Linked list for connection tracking
    Connection* next;
    Connection* prev;
};

// Timer structure
struct Timer {
    int             fd;        // timerfd
    TimerCallback   callback;
    void*           userdata;
    Timer*          next;
};

// Listener structure (internal)
typedef struct Listener {
    int              fd;
    AcceptCallback   on_accept;
    void*            userdata;
    void*            epoll_data;  // Owned EpollData pointer
    struct Listener* next;
} Listener;

// Event loop structure
struct EventLoop {
    int          epoll_fd;
    bool         running;

    // Active connections (doubly-linked list)
    Connection*  conn_head;
    int          conn_count;
    int          max_connections;  // Max allowed connections (0 = unlimited)

    // Listeners
    Listener*    listener_head;

    // Timers
    Timer*       timer_head;
};

// Event loop lifecycle
bool event_loop_init(EventLoop* loop);
void event_loop_destroy(EventLoop* loop);

// Run the event loop (blocks until event_loop_stop is called)
void event_loop_run(EventLoop* loop);

// Signal the event loop to stop
void event_loop_stop(EventLoop* loop);

// Create a listening socket on the given port
// Returns true on success, false on failure
bool event_loop_listen(EventLoop* loop, int port,
                       AcceptCallback on_accept, void* userdata);

// Queue data for writing on a connection
// Enables EPOLLOUT interest
bool event_loop_conn_write(EventLoop* loop, Connection* conn,
                           const void* data, size_t len);

// Close a connection
void event_loop_conn_close(EventLoop* loop, Connection* conn);

// Add a one-shot timer (fires after ms milliseconds)
// Returns a Timer pointer, or NULL on failure
Timer* event_loop_add_timer(EventLoop* loop, uint64_t ms,
                            TimerCallback callback, void* userdata);

// Cancel and free a timer
void event_loop_cancel_timer(EventLoop* loop, Timer* timer);

#endif // VEK_EVENT_LOOP_H
