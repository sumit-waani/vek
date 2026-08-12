#ifndef VEK_HTTP_SERVER_H
#define VEK_HTTP_SERVER_H

#include "event_loop.h"
#include "http_parser.h"
#include "router.h"

// Forward declarations
typedef struct HttpServer HttpServer;

// Route handler callback type
typedef void (*HttpHandlerFn)(HttpServer* server, Connection* conn,
                              HttpRequest* req, RouteParams* params,
                              void* userdata);

// Per-connection state
typedef struct {
    bool        keep_alive;
    HttpServer* server;
    Timer*      timeout;       // Request timeout timer (NULL if not set)
} HttpConnState;

// HTTP server
struct HttpServer {
    EventLoop  loop;
    Router     router;
    int        port;
    uint64_t   request_timeout_ms;  // Per-connection request timeout (0 = disabled)

    // User-level handler and userdata
    HttpHandlerFn handler;
    void*         handler_userdata;
};

// Initialize the HTTP server (does not start it)
bool http_server_init(HttpServer* server, int port);

// Destroy the HTTP server and free resources
void http_server_destroy(HttpServer* server);

// Add a route with a handler
void http_server_add_route(HttpServer* server, int method, const char* path,
                           void* handler_data);

// Start the server (blocks, runs the event loop)
bool http_server_start(HttpServer* server);

// Stop the server
void http_server_stop(HttpServer* server);

// Set the global request handler (called when a route matches)
void http_server_set_handler(HttpServer* server, HttpHandlerFn handler,
                             void* userdata);

// Write an HTTP response to a connection
void http_response_send(HttpServer* server, Connection* conn,
                        int status, const char* status_text,
                        const char* content_type,
                        const char* body, size_t body_len,
                        bool keep_alive);

#endif // VEK_HTTP_SERVER_H
