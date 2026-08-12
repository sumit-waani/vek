#define _GNU_SOURCE

#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>

// ---- URL parser ----

typedef struct {
    char host[256];
    char path[2048];
    int port;
    bool is_https;
} ParsedURL;

static bool parse_url(const char* url, ParsedURL* out) {
    memset(out, 0, sizeof(ParsedURL));
    out->port = 80;

    const char* p = url;
    if (strncmp(p, "https://", 8) == 0) {
        out->is_https = true;
        out->port = 443;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        out->is_https = false;
        p += 7;
    } else {
        return false;
    }

    // Extract host (and optional port)
    const char* host_start = p;
    const char* host_end = NULL;
    const char* port_start = NULL;

    while (*p && *p != '/' && *p != ':') p++;

    if (*p == ':') {
        host_end = p;
        p++;
        port_start = p;
        while (*p && *p != '/') p++;
        out->port = atoi(port_start);
    } else {
        host_end = p;
    }

    size_t host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= sizeof(out->host)) return false;
    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';

    // Path
    if (*p == '/') {
        strncpy(out->path, p, sizeof(out->path) - 1);
    } else {
        strcpy(out->path, "/");
    }

    return true;
}

// ---- TCP connection ----

static int tcp_connect(const char* host, int port, int timeout_sec) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int err = getaddrinfo(host, port_str, &hints, &res);
    if (err != 0) return -1;

    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        // Set non-blocking for connect timeout
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int ret = connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (ret == 0) {
            // Connected immediately
            fcntl(fd, F_SETFL, flags); // restore blocking
            break;
        }
        if (errno == EINPROGRESS) {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            int poll_timeout = (timeout_sec > 0) ? timeout_sec * 1000 : 5000;
            ret = poll(&pfd, 1, poll_timeout);
            if (ret > 0) {
                int so_error = 0;
                socklen_t len = sizeof(so_error);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
                if (so_error == 0) {
                    fcntl(fd, F_SETFL, flags); // restore blocking
                    break;
                }
            }
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

// ---- HTTP request/response ----

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} Buffer;

static void buf_init(Buffer* b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static void buf_append(Buffer* b, const char* data, size_t len) {
    if (b->len + len + 1 > b->cap) {
        size_t new_cap = (b->cap < 256) ? 256 : b->cap * 2;
        while (new_cap < b->len + len + 1) new_cap *= 2;
        b->data = (char*)realloc(b->data, new_cap);
        b->cap = new_cap;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = '\0';
}

static void buf_free(Buffer* b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static bool recv_all(int fd, Buffer* buf, int timeout_sec) {
    char tmp[4096];
    int poll_timeout = (timeout_sec > 0) ? timeout_sec * 1000 : 10000;

    for (;;) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ret = poll(&pfd, 1, poll_timeout);
        if (ret <= 0) break; // timeout or error

        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf_append(buf, tmp, (size_t)n);
    }
    return buf->len > 0;
}

// Parse HTTP response: status code, headers map, body string
static Value build_response(const char* raw, size_t raw_len) {
    // Find status line end
    const char* line_end = strstr(raw, "\r\n");
    if (!line_end) return VAL_NIL;

    // Parse "HTTP/1.x STATUS_CODE ..."
    int status = 0;
    if (strncmp(raw, "HTTP/", 5) == 0) {
        const char* sp = strchr(raw, ' ');
        if (sp && sp < line_end) {
            status = atoi(sp + 1);
        }
    }
    if (status == 0) return VAL_NIL;

    // Parse headers
    ObjMap* headers = obj_map_new();
    gc_push_root(OBJ_VAL(headers));

    const char* hdr_start = line_end + 2;
    const char* body_start = NULL;

    size_t content_length = 0;
    bool has_content_length = false;
    bool chunked = false;

    while (hdr_start < raw + raw_len) {
        const char* hdr_end = strstr(hdr_start, "\r\n");
        if (!hdr_end) break;
        if (hdr_end == hdr_start) {
            // Empty line: end of headers
            body_start = hdr_end + 2;
            break;
        }

        // Parse "Key: Value"
        const char* colon = memchr(hdr_start, ':', (size_t)(hdr_end - hdr_start));
        if (colon) {
            size_t key_len = (size_t)(colon - hdr_start);
            const char* val_start = colon + 1;
            while (val_start < hdr_end && *val_start == ' ') val_start++;
            size_t val_len = (size_t)(hdr_end - val_start);

            // Lowercase key for consistent access
            char key_lower[256];
            size_t kl = key_len < 255 ? key_len : 255;
            for (size_t i = 0; i < kl; i++) {
                key_lower[i] = (char)(hdr_start[i] >= 'A' && hdr_start[i] <= 'Z'
                    ? hdr_start[i] + 32 : hdr_start[i]);
            }
            key_lower[kl] = '\0';

            ObjString* hk = obj_string_new(key_lower, (uint32_t)kl);
            gc_push_root(OBJ_VAL(hk));
            ObjString* hv = obj_string_new(val_start, (uint32_t)val_len);
            gc_push_root(OBJ_VAL(hv));
            obj_map_set(headers, hk, OBJ_VAL(hv));
            gc_pop_root();
            gc_pop_root();

            // Check content-length
            if (kl == 14 && memcmp(key_lower, "content-length", 14) == 0) {
                content_length = (size_t)atol(val_start);
                has_content_length = true;
            }
            // Check transfer-encoding
            if (kl == 17 && memcmp(key_lower, "transfer-encoding", 17) == 0) {
                if (val_len >= 7 && strstr(val_start, "chunked")) {
                    chunked = true;
                }
            }
        }

        hdr_start = hdr_end + 2;
    }

    // Extract body
    const char* body = "";
    size_t body_len = 0;

    if (body_start && body_start < raw + raw_len) {
        size_t remaining = raw_len - (size_t)(body_start - raw);

        if (chunked) {
            // Decode chunked transfer encoding
            Buffer decoded;
            buf_init(&decoded);
            const char* cp = body_start;
            const char* end = raw + raw_len;
            while (cp < end) {
                // Read chunk size (hex)
                char* chunk_end;
                unsigned long chunk_size = strtoul(cp, &chunk_end, 16);
                if (chunk_end == cp) break;
                // Skip to data after \r\n
                const char* data_start = strstr(cp, "\r\n");
                if (!data_start) break;
                data_start += 2;
                if (chunk_size == 0) break;
                if (data_start + chunk_size > end) {
                    chunk_size = (unsigned long)(end - data_start);
                }
                buf_append(&decoded, data_start, chunk_size);
                cp = data_start + chunk_size;
                if (cp + 2 <= end && cp[0] == '\r' && cp[1] == '\n') cp += 2;
            }
            ObjString* body_str = obj_string_new(decoded.data ? decoded.data : "", (uint32_t)decoded.len);
            gc_push_root(OBJ_VAL(body_str));

            ObjMap* result = obj_map_new();
            gc_push_root(OBJ_VAL(result));

            ObjString* k_status = obj_string_new("status", 6);
            gc_push_root(OBJ_VAL(k_status));
            obj_map_set(result, k_status, INT_VAL(status));
            gc_pop_root();

            ObjString* k_body = obj_string_new("body", 4);
            gc_push_root(OBJ_VAL(k_body));
            obj_map_set(result, k_body, OBJ_VAL(body_str));
            gc_pop_root();

            ObjString* k_headers = obj_string_new("headers", 7);
            gc_push_root(OBJ_VAL(k_headers));
            obj_map_set(result, k_headers, OBJ_VAL(headers));
            gc_pop_root();

            gc_pop_root(); // result
            gc_pop_root(); // body_str
            gc_pop_root(); // headers

            buf_free(&decoded);
            return OBJ_VAL(result);
        } else if (has_content_length) {
            body = body_start;
            body_len = (content_length < remaining) ? content_length : remaining;
        } else {
            body = body_start;
            body_len = remaining;
        }
    }

    ObjString* body_str = obj_string_new(body, (uint32_t)body_len);
    gc_push_root(OBJ_VAL(body_str));

    ObjMap* result = obj_map_new();
    gc_push_root(OBJ_VAL(result));

    ObjString* k_status = obj_string_new("status", 6);
    gc_push_root(OBJ_VAL(k_status));
    obj_map_set(result, k_status, INT_VAL(status));
    gc_pop_root();

    ObjString* k_body = obj_string_new("body", 4);
    gc_push_root(OBJ_VAL(k_body));
    obj_map_set(result, k_body, OBJ_VAL(body_str));
    gc_pop_root();

    ObjString* k_headers = obj_string_new("headers", 7);
    gc_push_root(OBJ_VAL(k_headers));
    obj_map_set(result, k_headers, OBJ_VAL(headers));
    gc_pop_root();

    gc_pop_root(); // result
    gc_pop_root(); // body_str
    gc_pop_root(); // headers

    return OBJ_VAL(result);
}

// Extract optional config map (headers, timeout)
static void extract_config(Value config_val, ObjMap** extra_headers, int* timeout) {
    *extra_headers = NULL;
    *timeout = 5;
    if (!IS_MAP(config_val)) return;

    ObjMap* config = AS_MAP(config_val);
    Value val;

    ObjString* k_timeout = obj_string_new("timeout", 7);
    if (obj_map_get(config, k_timeout, &val) && IS_INT(val)) {
        *timeout = (int)AS_INT(val);
    }

    ObjString* k_headers = obj_string_new("headers", 7);
    if (obj_map_get(config, k_headers, &val) && IS_MAP(val)) {
        *extra_headers = AS_MAP(val);
    }
}

// Build HTTP request string
static void build_request(Buffer* req, const char* method, ParsedURL* url,
                          const char* body_data, size_t body_len,
                          ObjMap* extra_headers) {
    char line[4096];
    int n;

    n = snprintf(line, sizeof(line), "%s %s HTTP/1.1\r\n", method, url->path);
    buf_append(req, line, (size_t)n);

    n = snprintf(line, sizeof(line), "Host: %s\r\n", url->host);
    buf_append(req, line, (size_t)n);

    buf_append(req, "Connection: close\r\n", 19);

    if (body_data && body_len > 0) {
        n = snprintf(line, sizeof(line), "Content-Length: %zu\r\n", body_len);
        buf_append(req, line, (size_t)n);
    }

    // Add extra headers
    if (extra_headers) {
        for (uint32_t i = 0; i < extra_headers->capacity; i++) {
            if (extra_headers->entries[i].key == NULL) continue;
            if (extra_headers->entries[i].key == MAP_TOMBSTONE) continue;
            ObjString* hk = extra_headers->entries[i].key;
            Value hv_val = extra_headers->entries[i].value;
            if (IS_STRING(hv_val)) {
                ObjString* hv = AS_STRING(hv_val);
                n = snprintf(line, sizeof(line), "%.*s: %.*s\r\n",
                    (int)hk->length, hk->data, (int)hv->length, hv->data);
                buf_append(req, line, (size_t)n);
            }
        }
    }

    buf_append(req, "\r\n", 2);

    if (body_data && body_len > 0) {
        buf_append(req, body_data, body_len);
    }
}

// Core HTTP request function
static Value do_http_request(const char* method, int argc, Value* args,
                             bool has_body, ObjMap* default_headers) {
    if (argc < 1 || !IS_STRING(args[0])) return VAL_NIL;

    ObjString* url_str = AS_STRING(args[0]);
    ParsedURL url;
    if (!parse_url(url_str->data, &url)) return VAL_NIL;

    // HTTPS not supported in v1
    if (url.is_https) return VAL_NIL;

    const char* body_data = NULL;
    size_t body_len = 0;
    int config_idx = 1;

    if (has_body && argc >= 2 && IS_STRING(args[1])) {
        ObjString* body_str = AS_STRING(args[1]);
        body_data = body_str->data;
        body_len = body_str->length;
        config_idx = 2;
    }

    ObjMap* extra_headers = NULL;
    int timeout = 5;

    if (argc > config_idx) {
        extract_config(args[config_idx], &extra_headers, &timeout);
    }

    // Merge default_headers with extra_headers
    if (default_headers && !extra_headers) {
        extra_headers = default_headers;
    } else if (default_headers && extra_headers) {
        // extra_headers takes precedence; add defaults that are missing
        for (uint32_t i = 0; i < default_headers->capacity; i++) {
            if (default_headers->entries[i].key == NULL) continue;
            if (default_headers->entries[i].key == MAP_TOMBSTONE) continue;
            Value existing;
            if (!obj_map_get(extra_headers, default_headers->entries[i].key, &existing)) {
                obj_map_set(extra_headers, default_headers->entries[i].key,
                           default_headers->entries[i].value);
            }
        }
    }

    // Connect
    int fd = tcp_connect(url.host, url.port, timeout);
    if (fd < 0) return VAL_NIL;

    // Build request
    Buffer req;
    buf_init(&req);
    build_request(&req, method, &url, body_data, body_len, extra_headers);

    // Send
    ssize_t sent = send(fd, req.data, req.len, 0);
    buf_free(&req);
    if (sent <= 0) {
        close(fd);
        return VAL_NIL;
    }

    // Receive
    Buffer resp;
    buf_init(&resp);
    recv_all(fd, &resp, timeout);
    close(fd);

    if (resp.len == 0) {
        buf_free(&resp);
        return VAL_NIL;
    }

    Value result = build_response(resp.data, resp.len);
    buf_free(&resp);
    return result;
}

// ---- Native functions ----

// http.get(url [, config])
static Value native_http_get(int argc, Value* args) {
    return do_http_request("GET", argc, args, false, NULL);
}

// http.post(url, body [, config])
static Value native_http_post(int argc, Value* args) {
    return do_http_request("POST", argc, args, true, NULL);
}

// http.put(url, body [, config])
static Value native_http_put(int argc, Value* args) {
    return do_http_request("PUT", argc, args, true, NULL);
}

// http.delete(url [, config])
static Value native_http_delete(int argc, Value* args) {
    return do_http_request("DELETE", argc, args, false, NULL);
}

// http.json_get(url [, config])
static Value native_http_json_get(int argc, Value* args) {
    ObjMap* hdrs = obj_map_new();
    gc_push_root(OBJ_VAL(hdrs));
    ObjString* ct_key = obj_string_new("Content-Type", 12);
    gc_push_root(OBJ_VAL(ct_key));
    ObjString* ct_val = obj_string_new("application/json", 16);
    gc_push_root(OBJ_VAL(ct_val));
    obj_map_set(hdrs, ct_key, OBJ_VAL(ct_val));

    ObjString* ac_key = obj_string_new("Accept", 6);
    gc_push_root(OBJ_VAL(ac_key));
    ObjString* ac_val = obj_string_new("application/json", 16);
    gc_push_root(OBJ_VAL(ac_val));
    obj_map_set(hdrs, ac_key, OBJ_VAL(ac_val));

    Value result = do_http_request("GET", argc, args, false, hdrs);

    gc_pop_root(); // ac_val
    gc_pop_root(); // ac_key
    gc_pop_root(); // ct_val
    gc_pop_root(); // ct_key
    gc_pop_root(); // hdrs
    return result;
}

// http.json_post(url, body [, config])
static Value native_http_json_post(int argc, Value* args) {
    ObjMap* hdrs = obj_map_new();
    gc_push_root(OBJ_VAL(hdrs));
    ObjString* ct_key = obj_string_new("Content-Type", 12);
    gc_push_root(OBJ_VAL(ct_key));
    ObjString* ct_val = obj_string_new("application/json", 16);
    gc_push_root(OBJ_VAL(ct_val));
    obj_map_set(hdrs, ct_key, OBJ_VAL(ct_val));

    ObjString* ac_key = obj_string_new("Accept", 6);
    gc_push_root(OBJ_VAL(ac_key));
    ObjString* ac_val = obj_string_new("application/json", 16);
    gc_push_root(OBJ_VAL(ac_val));
    obj_map_set(hdrs, ac_key, OBJ_VAL(ac_val));

    Value result = do_http_request("POST", argc, args, true, hdrs);

    gc_pop_root(); // ac_val
    gc_pop_root(); // ac_key
    gc_pop_root(); // ct_val
    gc_pop_root(); // ct_key
    gc_pop_root(); // hdrs
    return result;
}

void stdlib_http_client_init(ObjMap* pkg) {
    stdlib_register(pkg, "get", native_http_get, -1);
    stdlib_register(pkg, "post", native_http_post, -1);
    stdlib_register(pkg, "put", native_http_put, -1);
    stdlib_register(pkg, "delete", native_http_delete, -1);
    stdlib_register(pkg, "json_get", native_http_json_get, -1);
    stdlib_register(pkg, "json_post", native_http_json_post, -1);
}
