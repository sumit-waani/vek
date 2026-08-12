/*
 * http_server.c - HTTP/1.1 server built on the event loop.
 * Ties together the event loop, HTTP parser, and router.
 */

#define _GNU_SOURCE

#include "http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Default request timeout (30 seconds)
#define DEFAULT_REQUEST_TIMEOUT_MS 30000

// ---- Response formatting ----

static const char* status_text_for(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default:  return "Unknown";
    }
}

static void format_date(char* buf, size_t buflen) {
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(buf, buflen, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

void http_response_send(HttpServer* server, Connection* conn,
                        int status, const char* st_text,
                        const char* content_type,
                        const char* body, size_t body_len,
                        bool keep_alive) {
    if (!st_text) st_text = status_text_for(status);

    char date_buf[64];
    format_date(date_buf, sizeof(date_buf));

    // Calculate total response size (header + body in single buffer)
    size_t hdr_size = 512 + (content_type ? strlen(content_type) : 0);
    size_t total_size = hdr_size + body_len;
    char* resp_buf = malloc(total_size);
    if (!resp_buf) return;

    int hdr_len = snprintf(resp_buf, hdr_size,
        "HTTP/1.1 %d %s\r\n"
        "Date: %s\r\n"
        "Content-Length: %zu\r\n"
        "Content-Type: %s\r\n"
        "Connection: %s\r\n"
        "\r\n",
        status, st_text,
        date_buf,
        body_len,
        content_type ? content_type : "text/plain",
        keep_alive ? "keep-alive" : "close");

    // Append body to same buffer
    if (body && body_len > 0) {
        memcpy(resp_buf + hdr_len, body, body_len);
    }

    // Single write call: avoids use-after-free if on_write_done fires synchronously
    event_loop_conn_write(&server->loop, conn, resp_buf, (size_t)hdr_len + body_len);
    free(resp_buf);
}

// ---- 404 Not Found response ----

static void send_404(HttpServer* server, Connection* conn, bool keep_alive) {
    const char* body = "404 Not Found";
    http_response_send(server, conn, 404, "Not Found", "text/plain",
                       body, strlen(body), keep_alive);
}

// ---- Connection callbacks ----

// Timeout callback: close connection if request not received in time
static void on_request_timeout(EventLoop* loop, void* userdata) {
    Connection* conn = (Connection*)userdata;
    if (!conn || conn->state == CONN_CLOSED) return;

    HttpConnState* state = (HttpConnState*)conn->userdata;
    if (state) {
        state->timeout = NULL; // Timer already fired and was freed
        free(state);
        conn->userdata = NULL;
    }

    // Send 408 Request Timeout and close
    // We need access to the server to send a response, but we can just close
    event_loop_conn_close(loop, conn);
}

static void on_write_done(EventLoop* loop, Connection* conn, void* userdata) {
    (void)loop;
    HttpConnState* state = (HttpConnState*)userdata;
    if (!state) return;

    if (!state->keep_alive) {
        // Cancel timeout if active
        if (state->timeout) {
            event_loop_cancel_timer(loop, state->timeout);
            state->timeout = NULL;
        }
        // Free state before closing; on_close will be a no-op since userdata is NULL
        free(state);
        conn->userdata = NULL;
        event_loop_conn_close(loop, conn);
    }
    // If keep-alive, wait for more data (already registered for EPOLLIN)
}

static void on_close(EventLoop* loop, Connection* conn, void* userdata) {
    HttpConnState* state = (HttpConnState*)userdata;
    if (!state) return;

    if (state->timeout) {
        event_loop_cancel_timer(loop, state->timeout);
        state->timeout = NULL;
    }
    free(state);
    conn->userdata = NULL;
}

static void on_data(EventLoop* loop, Connection* conn, void* userdata) {
    HttpConnState* state = (HttpConnState*)userdata;
    if (!state) return;
    HttpServer* server = state->server;

    // Try to parse a complete HTTP request from the read buffer
    HttpRequest req;
    HttpParseResult result = http_parse_request(
        (const char*)conn->read_buf.data,
        conn->read_buf.len,
        &req);

    if (result == HTTP_PARSE_INCOMPLETE) {
        // Wait for more data
        return;
    }

    // Request received (or error) - cancel the timeout
    if (state->timeout) {
        event_loop_cancel_timer(loop, state->timeout);
        state->timeout = NULL;
    }

    if (result == HTTP_PARSE_ERROR) {
        // Send 400 and close
        const char* body = "400 Bad Request";
        http_response_send(server, conn, 400, "Bad Request", "text/plain",
                           body, strlen(body), false);
        state->keep_alive = false;
        return;
    }

    // Successfully parsed request
    state->keep_alive = req.keep_alive;

    // Match route
    RouteParams params;
    void* handler = router_match(&server->router, (int)req.method,
                                 req.path, req.path_len, &params);

    if (handler && server->handler) {
        // Call the user handler
        server->handler(server, conn, &req, &params, handler);
    } else {
        send_404(server, conn, req.keep_alive);
    }

    // Consume parsed bytes from read buffer
    size_t consumed = req.bytes_consumed;
    if (consumed < conn->read_buf.len) {
        size_t remaining = conn->read_buf.len - consumed;
        memmove(conn->read_buf.data, conn->read_buf.data + consumed, remaining);
        conn->read_buf.len = remaining;
    } else {
        conn->read_buf.len = 0;
    }

    // Re-arm keep-alive timeout for next request
    if (state->keep_alive && server->request_timeout_ms > 0) {
        state->timeout = event_loop_add_timer(loop, server->request_timeout_ms,
                                              on_request_timeout, conn);
    }

    (void)loop;
}

static void on_accept(EventLoop* loop, Connection* conn, void* userdata) {
    HttpServer* server = (HttpServer*)userdata;

    HttpConnState* state = calloc(1, sizeof(HttpConnState));
    if (!state) {
        event_loop_conn_close(loop, conn);
        return;
    }

    state->keep_alive = true;
    state->server = server;
    state->timeout = NULL;

    conn->userdata = state;
    conn->on_data = on_data;
    conn->on_write_done = on_write_done;
    conn->on_close = on_close;

    // Set request timeout if configured
    if (server->request_timeout_ms > 0) {
        state->timeout = event_loop_add_timer(loop, server->request_timeout_ms,
                                              on_request_timeout, conn);
    }
}

// ---- Public API ----

bool http_server_init(HttpServer* server, int port) {
    memset(server, 0, sizeof(HttpServer));
    server->port = port;
    server->handler = NULL;
    server->handler_userdata = NULL;
    server->request_timeout_ms = DEFAULT_REQUEST_TIMEOUT_MS;

    router_init(&server->router);

    if (!event_loop_init(&server->loop)) {
        return false;
    }

    return true;
}

void http_server_destroy(HttpServer* server) {
    router_destroy(&server->router);
    event_loop_destroy(&server->loop);
}

void http_server_add_route(HttpServer* server, int method, const char* path,
                           void* handler_data) {
    router_add(&server->router, method, path, handler_data);
}

void http_server_set_handler(HttpServer* server, HttpHandlerFn handler,
                             void* userdata) {
    server->handler = handler;
    server->handler_userdata = userdata;
}

bool http_server_start(HttpServer* server) {
    if (!event_loop_listen(&server->loop, server->port, on_accept, server)) {
        return false;
    }
    event_loop_run(&server->loop);
    return true;
}

void http_server_stop(HttpServer* server) {
    event_loop_stop(&server->loop);
}
