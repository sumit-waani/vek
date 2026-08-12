/*
 * stdlib_http_server.c - HTTP server bindings for the vek language.
 * Exposes a "server" package with: get, post, put, delete, listen, json.
 */

#define _GNU_SOURCE

#include "vek_stdlib.h"
#include "http_server.h"
#include "vm.h"
#include "gc.h"
#include "object.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>

// Global HTTP server instance (one per process)
static HttpServer g_server;
static bool g_server_initialized = false;

// Forward declarations
static void on_accept_vek(EventLoop* loop, Connection* conn, void* userdata);
static void on_data_vek(EventLoop* loop, Connection* conn, void* userdata);
static void on_write_done_vek(EventLoop* loop, Connection* conn, void* userdata);
static void on_close_vek(EventLoop* loop, Connection* conn, void* userdata);
static void vek_http_handler(HttpServer* server, Connection* conn,
                             HttpRequest* req, RouteParams* params,
                             void* userdata);

// Route handler entry: stores the vek closure for a route
typedef struct RouteEntry {
    Value closure;      // The vek closure to call
    struct RouteEntry* next;
} RouteEntry;

// Linked list of all route entries (to keep them rooted for GC)
static RouteEntry* g_routes = NULL;

// Create a route entry and pin the closure
static RouteEntry* create_route_entry(Value closure) {
    RouteEntry* entry = malloc(sizeof(RouteEntry));
    if (!entry) return NULL;
    entry->closure = closure;
    entry->next = g_routes;
    g_routes = entry;

    // Pin the closure so GC doesn't collect it
    if (IS_PTR(closure)) {
        vm_pin((ObjHeader*)AS_PTR(closure));
    }
    return entry;
}

// Ensure the server is initialized
static bool ensure_server(void) {
    if (!g_server_initialized) {
        // Will be initialized when listen is called with a port
        return true;
    }
    return true;
}

// Helper to register a route for a given method
static Value register_route(int method, int arg_count, Value* args) {
    if (arg_count < 2) return VAL_NIL;
    if (!IS_STRING(args[0])) return VAL_NIL;
    if (!IS_CLOSURE(args[1]) && !IS_NATIVE(args[1])) return VAL_NIL;

    ObjString* path = AS_STRING(args[0]);

    // Create a null-terminated copy of the path
    char* path_cstr = malloc(path->length + 1);
    memcpy(path_cstr, path->data, path->length);
    path_cstr[path->length] = '\0';

    RouteEntry* entry = create_route_entry(args[1]);
    if (!entry) {
        free(path_cstr);
        return VAL_NIL;
    }

    ensure_server();

    router_add(&g_server.router, method, path_cstr, entry);
    free(path_cstr);

    return VAL_NIL;
}

// server.get(path, handler)
static Value native_server_get(int arg_count, Value* args) {
    return register_route(HTTP_GET, arg_count, args);
}

// server.post(path, handler)
static Value native_server_post(int arg_count, Value* args) {
    return register_route(HTTP_POST, arg_count, args);
}

// server.put(path, handler)
static Value native_server_put(int arg_count, Value* args) {
    return register_route(HTTP_PUT, arg_count, args);
}

// server.delete(path, handler)
static Value native_server_delete(int arg_count, Value* args) {
    return register_route(HTTP_DELETE, arg_count, args);
}

// server.patch(path, handler)
static Value native_server_patch(int arg_count, Value* args) {
    return register_route(HTTP_PATCH, arg_count, args);
}

// Build a vek request map from the parsed HTTP request and route params
static Value build_request_map(HttpRequest* req, RouteParams* params) {
    ObjMap* rmap = obj_map_new();
    gc_push_root(OBJ_VAL(rmap));

    // method
    const char* method_s = http_method_str(req->method);
    ObjString* method_key = obj_string_new("method", 6);
    ObjString* method_val = obj_string_new(method_s, (uint32_t)strlen(method_s));
    obj_map_set(rmap, method_key, OBJ_VAL(method_val));

    // path
    ObjString* path_key = obj_string_new("path", 4);
    ObjString* path_val = obj_string_new(req->path, req->path_len);
    obj_map_set(rmap, path_key, OBJ_VAL(path_val));

    // query
    ObjString* query_key = obj_string_new("query", 5);
    if (req->query_string && req->query_len > 0) {
        ObjString* query_val = obj_string_new(req->query_string, req->query_len);
        obj_map_set(rmap, query_key, OBJ_VAL(query_val));
    } else {
        obj_map_set(rmap, query_key, OBJ_VAL(obj_string_new("", 0)));
    }

    // body
    ObjString* body_key = obj_string_new("body", 4);
    if (req->body && req->body_len > 0) {
        ObjString* body_val = obj_string_new(req->body, (uint32_t)req->body_len);
        obj_map_set(rmap, body_key, OBJ_VAL(body_val));
    } else {
        obj_map_set(rmap, body_key, OBJ_VAL(obj_string_new("", 0)));
    }

    // headers map
    ObjMap* headers_map = obj_map_new();
    gc_push_root(OBJ_VAL(headers_map));
    for (int i = 0; i < req->header_count; i++) {
        // Lowercase the header name for consistency
        uint32_t nlen = req->headers[i].name_len;
        char stack_buf[256];
        char* name_buf = stack_buf;

        // Use heap allocation for unusually long header names
        if (nlen >= sizeof(stack_buf)) {
            name_buf = malloc(nlen + 1);
            if (!name_buf) continue; // skip this header on alloc failure
        }

        for (uint32_t j = 0; j < nlen; j++) {
            name_buf[j] = (char)tolower((unsigned char)req->headers[i].name[j]);
        }
        name_buf[nlen] = '\0';

        ObjString* hname = obj_string_new(name_buf, nlen);
        ObjString* hval = obj_string_new(req->headers[i].value,
                                         req->headers[i].value_len);
        obj_map_set(headers_map, hname, OBJ_VAL(hval));

        if (name_buf != stack_buf) {
            free(name_buf);
        }
    }
    ObjString* headers_key = obj_string_new("headers", 7);
    obj_map_set(rmap, headers_key, OBJ_VAL(headers_map));
    gc_pop_root(); // headers_map

    // params map
    ObjMap* params_map = obj_map_new();
    gc_push_root(OBJ_VAL(params_map));
    for (int i = 0; i < params->count; i++) {
        ObjString* pname = obj_string_new(params->params[i].name,
                                          params->params[i].name_len);
        ObjString* pval = obj_string_new(params->params[i].value,
                                         params->params[i].value_len);
        obj_map_set(params_map, pname, OBJ_VAL(pval));
    }
    ObjString* params_key = obj_string_new("params", 6);
    obj_map_set(rmap, params_key, OBJ_VAL(params_map));
    gc_pop_root(); // params_map

    gc_pop_root(); // rmap
    return OBJ_VAL(rmap);
}

// Handle an HTTP request by calling the vek closure
static void vek_http_handler(HttpServer* server, Connection* conn,
                             HttpRequest* req, RouteParams* params,
                             void* userdata) {
    RouteEntry* entry = (RouteEntry*)userdata;
    if (!entry) return;

    // Build the request map
    Value req_map = build_request_map(req, params);
    gc_push_root(req_map);

    // Call the vek closure
    vm_push(entry->closure);
    vm_push(req_map);
    Value result = vm_call(entry->closure, 1);

    gc_pop_root(); // req_map

    // Check if the handler had a runtime error
    if (vm.had_error) {
        // Reset the error state so subsequent requests can proceed
        vm.had_error = false;
        vm.error_msg[0] = '\0';

        const char* err_body = "500 Internal Server Error";
        http_response_send(server, conn, 500, "Internal Server Error",
                           "text/plain", err_body, strlen(err_body), req->keep_alive);
        return;
    }

    // Process the result
    bool keep_alive = req->keep_alive;

    if (IS_STRING(result)) {
        // String shorthand: 200 OK text/html
        ObjString* body = AS_STRING(result);
        http_response_send(server, conn, 200, "OK", "text/html",
                           body->data, body->length, keep_alive);
    } else if (IS_MAP(result)) {
        // Response map: {status: int, body: string, headers: map}
        ObjMap* resp = AS_MAP(result);
        int status = 200;
        const char* body = "";
        size_t body_len = 0;
        const char* content_type = "text/plain";

        // Get status
        ObjString* status_key = obj_string_new("status", 6);
        Value status_val;
        if (obj_map_get(resp, status_key, &status_val) && IS_INT(status_val)) {
            status = (int)AS_INT(status_val);
        }

        // Get body
        ObjString* body_key = obj_string_new("body", 4);
        Value body_val;
        if (obj_map_get(resp, body_key, &body_val) && IS_STRING(body_val)) {
            ObjString* bstr = AS_STRING(body_val);
            body = bstr->data;
            body_len = bstr->length;
        }

        // Get content-type from headers map
        ObjString* headers_key = obj_string_new("headers", 7);
        Value headers_val;
        if (obj_map_get(resp, headers_key, &headers_val) && IS_MAP(headers_val)) {
            ObjMap* hdrs = AS_MAP(headers_val);
            ObjString* ct_key = obj_string_new("content-type", 12);
            Value ct_val;
            if (obj_map_get(hdrs, ct_key, &ct_val) && IS_STRING(ct_val)) {
                content_type = AS_STRING(ct_val)->data;
            }
        }

        http_response_send(server, conn, status, NULL, content_type,
                           body, body_len, keep_alive);
    } else {
        // Nil or other: send empty 200 (handler returned no explicit response)
        http_response_send(server, conn, 200, "OK", "text/plain",
                           "", 0, keep_alive);
    }
}

// Signal handler for graceful shutdown
static void sigint_handler(int sig) {
    (void)sig;
    if (g_server_initialized) {
        http_server_stop(&g_server);
    }
}

// Timeout callback for vek HTTP server connections
static void on_timeout_vek(EventLoop* loop, void* userdata) {
    Connection* conn = (Connection*)userdata;
    if (!conn || conn->state == CONN_CLOSED) return;

    HttpConnState* state = (HttpConnState*)conn->userdata;
    if (state) {
        state->timeout = NULL; // Timer already fired and was freed
        free(state);
        conn->userdata = NULL;
    }

    event_loop_conn_close(loop, conn);
}

// Accept callback for vek HTTP server
static void on_accept_vek(EventLoop* loop, Connection* conn, void* userdata) {
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
    conn->on_data = on_data_vek;
    conn->on_write_done = on_write_done_vek;
    conn->on_close = on_close_vek;

    // Set request timeout if configured
    if (server->request_timeout_ms > 0) {
        state->timeout = event_loop_add_timer(loop, server->request_timeout_ms,
                                              on_timeout_vek, conn);
    }
}

// Write done callback
static void on_write_done_vek(EventLoop* loop, Connection* conn, void* userdata) {
    HttpConnState* state = (HttpConnState*)userdata;
    if (!state) return;

    if (!state->keep_alive) {
        if (state->timeout) {
            event_loop_cancel_timer(loop, state->timeout);
            state->timeout = NULL;
        }
        // Free state before closing; on_close_vek will be a no-op since userdata is NULL
        free(state);
        conn->userdata = NULL;
        event_loop_conn_close(loop, conn);
    }
}

// Close callback - frees HttpConnState when connection is closed externally
static void on_close_vek(EventLoop* loop, Connection* conn, void* userdata) {
    HttpConnState* state = (HttpConnState*)userdata;
    if (!state) return;

    if (state->timeout) {
        event_loop_cancel_timer(loop, state->timeout);
        state->timeout = NULL;
    }
    free(state);
    conn->userdata = NULL;
}

// Data callback - parse HTTP request and dispatch to handler
static void on_data_vek(EventLoop* loop, Connection* conn, void* userdata) {
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
        return;
    }

    // Request received (or error) - cancel the timeout
    if (state->timeout) {
        event_loop_cancel_timer(loop, state->timeout);
        state->timeout = NULL;
    }

    if (result == HTTP_PARSE_ERROR) {
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

    if (handler) {
        vek_http_handler(server, conn, &req, &params, handler);
    } else {
        const char* body = "404 Not Found";
        http_response_send(server, conn, 404, "Not Found", "text/plain",
                           body, strlen(body), req.keep_alive);
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
                                              on_timeout_vek, conn);
    }

    (void)loop;
}

// server.listen(port)
static Value native_server_listen(int arg_count, Value* args) {
    int port = 3000; // default
    if (arg_count > 0 && IS_INT(args[0])) {
        port = (int)AS_INT(args[0]);
    }

    if (g_server_initialized) {
        http_server_destroy(&g_server);
    }

    // Save the router that already has routes registered
    Router saved_router = g_server.router;

    // Initialize the event loop (but not the router since we already have it)
    if (!event_loop_init(&g_server.loop)) {
        fprintf(stderr, "Failed to initialize event loop\n");
        g_server.router = saved_router; // restore
        return VAL_NIL;
    }

    g_server.port = port;
    g_server.router = saved_router;
    g_server.handler = NULL;
    g_server.handler_userdata = NULL;
    g_server.request_timeout_ms = 30000; // 30 second request timeout
    g_server_initialized = true;

    http_server_set_handler(&g_server, vek_http_handler, NULL);

    // Set up signal handling for graceful shutdown
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    fprintf(stderr, "Server listening on port %d\n", port);

    if (!event_loop_listen(&g_server.loop, port, on_accept_vek, &g_server)) {
        fprintf(stderr, "Failed to start HTTP server on port %d\n", port);
        return VAL_NIL;
    }

    event_loop_run(&g_server.loop);

    return VAL_NIL;
}

// server.json(data, status?) - helper to create a JSON response map
static Value native_server_json(int arg_count, Value* args) {
    if (arg_count < 1) return VAL_NIL;

    Value data = args[0];
    int status = 200;
    if (arg_count > 1 && IS_INT(args[1])) {
        status = (int)AS_INT(args[1]);
    }

    // We need json.encode - find it from globals
    ObjString* json_name = obj_string_new("json", 4);
    Value json_pkg_val;
    if (!obj_map_get(vm.globals, json_name, &json_pkg_val) || !IS_MAP(json_pkg_val)) {
        return VAL_NIL;
    }

    ObjMap* json_pkg = AS_MAP(json_pkg_val);
    ObjString* encode_name = obj_string_new("encode", 6);
    Value encode_val;
    if (!obj_map_get(json_pkg, encode_name, &encode_val)) {
        return VAL_NIL;
    }

    // Call json.encode(data)
    vm_push(encode_val);
    vm_push(data);
    Value json_str = vm_call(encode_val, 1);

    if (!IS_STRING(json_str)) return VAL_NIL;

    // Build response map: {status: status, body: json_str, headers: {"content-type": "application/json"}}
    ObjMap* resp = obj_map_new();
    gc_push_root(OBJ_VAL(resp));

    ObjString* status_key = obj_string_new("status", 6);
    obj_map_set(resp, status_key, INT_VAL(status));

    ObjString* body_key = obj_string_new("body", 4);
    obj_map_set(resp, body_key, json_str);

    ObjMap* headers = obj_map_new();
    gc_push_root(OBJ_VAL(headers));
    ObjString* ct_key = obj_string_new("content-type", 12);
    ObjString* ct_val = obj_string_new("application/json", 16);
    obj_map_set(headers, ct_key, OBJ_VAL(ct_val));

    ObjString* headers_key = obj_string_new("headers", 7);
    obj_map_set(resp, headers_key, OBJ_VAL(headers));
    gc_pop_root(); // headers
    gc_pop_root(); // resp

    return OBJ_VAL(resp);
}

// Initialize the server package
void stdlib_http_server_init(ObjMap* pkg) {
    // Initialize the router early (before routes are added)
    router_init(&g_server.router);

    stdlib_register(pkg, "get", native_server_get, -1);
    stdlib_register(pkg, "post", native_server_post, -1);
    stdlib_register(pkg, "put", native_server_put, -1);
    stdlib_register(pkg, "delete", native_server_delete, -1);
    stdlib_register(pkg, "patch", native_server_patch, -1);
    stdlib_register(pkg, "listen", native_server_listen, -1);
    stdlib_register(pkg, "json", native_server_json, -1);
}
