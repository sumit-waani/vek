/*
 * http_parser.c - Zero-copy HTTP/1.1 request parser.
 * Parses method, path, query string, headers, and body from a buffer.
 */

#include "http_parser.h"
#include <string.h>
#include <ctype.h>

// Maximum allowed request body size (1 MB)
#define MAX_BODY_SIZE (1024 * 1024)

// Case-insensitive comparison of fixed-length strings
static bool strncasecmp_eq(const char* a, uint32_t alen,
                           const char* b, uint32_t blen) {
    if (alen != blen) return false;
    for (uint32_t i = 0; i < alen; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
    }
    return true;
}

static HttpMethod parse_method(const char* str, uint32_t len) {
    switch (len) {
        case 3:
            if (memcmp(str, "GET", 3) == 0) return HTTP_GET;
            if (memcmp(str, "PUT", 3) == 0) return HTTP_PUT;
            break;
        case 4:
            if (memcmp(str, "POST", 4) == 0) return HTTP_POST;
            if (memcmp(str, "HEAD", 4) == 0) return HTTP_HEAD;
            break;
        case 5:
            if (memcmp(str, "PATCH", 5) == 0) return HTTP_PATCH;
            break;
        case 6:
            if (memcmp(str, "DELETE", 6) == 0) return HTTP_DELETE;
            break;
        case 7:
            if (memcmp(str, "OPTIONS", 7) == 0) return HTTP_OPTIONS;
            break;
    }
    return HTTP_METHOD_UNKNOWN;
}

const char* http_method_str(HttpMethod method) {
    switch (method) {
        case HTTP_GET:     return "GET";
        case HTTP_POST:    return "POST";
        case HTTP_PUT:     return "PUT";
        case HTTP_PATCH:   return "PATCH";
        case HTTP_DELETE:  return "DELETE";
        case HTTP_HEAD:    return "HEAD";
        case HTTP_OPTIONS: return "OPTIONS";
        default:           return "UNKNOWN";
    }
}

// Find CRLF in buffer. Returns pointer to CR, or NULL if not found.
static const char* find_crlf(const char* buf, size_t len) {
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') {
            return buf + i;
        }
    }
    return NULL;
}

HttpParseResult http_parse_request(const char* buf, size_t len, HttpRequest* req) {
    memset(req, 0, sizeof(HttpRequest));

    // Look for end of headers (CRLFCRLF)
    const char* header_end = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n') {
            header_end = buf + i;
            break;
        }
    }

    if (!header_end) {
        return HTTP_PARSE_INCOMPLETE;
    }

    const char* pos = buf;
    const char* end = buf + len;

    // Parse request line: METHOD SP PATH SP HTTP/x.y CRLF
    const char* line_end = find_crlf(pos, (size_t)(end - pos));
    if (!line_end) return HTTP_PARSE_ERROR;

    // Method
    const char* sp = memchr(pos, ' ', (size_t)(line_end - pos));
    if (!sp) return HTTP_PARSE_ERROR;

    req->method_str = pos;
    req->method_len = (uint32_t)(sp - pos);
    req->method = parse_method(pos, req->method_len);

    pos = sp + 1;

    // Path (and optional query string)
    sp = memchr(pos, ' ', (size_t)(line_end - pos));
    if (!sp) return HTTP_PARSE_ERROR;

    // Check for query string
    const char* qmark = memchr(pos, '?', (size_t)(sp - pos));
    if (qmark) {
        req->path = pos;
        req->path_len = (uint32_t)(qmark - pos);
        req->query_string = qmark + 1;
        req->query_len = (uint32_t)(sp - qmark - 1);
    } else {
        req->path = pos;
        req->path_len = (uint32_t)(sp - pos);
        req->query_string = NULL;
        req->query_len = 0;
    }

    pos = sp + 1;

    // HTTP version: HTTP/x.y
    if (line_end - pos < 8) return HTTP_PARSE_ERROR;
    if (memcmp(pos, "HTTP/", 5) != 0) return HTTP_PARSE_ERROR;
    req->version_major = pos[5] - '0';
    req->version_minor = pos[7] - '0';

    pos = line_end + 2; // Skip CRLF

    // Parse headers
    req->header_count = 0;
    req->content_length = 0;

    while (pos < header_end) {
        const char* hline_end = find_crlf(pos, (size_t)(end - pos));
        if (!hline_end) return HTTP_PARSE_ERROR;

        if (hline_end == pos) {
            // Empty line = end of headers
            pos = hline_end + 2;
            break;
        }

        // Find colon
        const char* colon = memchr(pos, ':', (size_t)(hline_end - pos));
        if (!colon) return HTTP_PARSE_ERROR;

        if (req->header_count >= HTTP_MAX_HEADERS) {
            return HTTP_PARSE_ERROR;
        }

        HttpHeader* h = &req->headers[req->header_count];
        h->name = pos;
        h->name_len = (uint32_t)(colon - pos);

        // Skip colon and optional whitespace
        const char* vstart = colon + 1;
        while (vstart < hline_end && *vstart == ' ') vstart++;

        h->value = vstart;
        h->value_len = (uint32_t)(hline_end - vstart);
        req->header_count++;

        pos = hline_end + 2;
    }

    // Skip past the final CRLF of the header section
    pos = header_end + 4;

    // Determine content_length and keep_alive from headers
    for (int i = 0; i < req->header_count; i++) {
        HttpHeader* h = &req->headers[i];
        if (strncasecmp_eq(h->name, h->name_len, "content-length", 14)) {
            req->content_length = 0;
            for (uint32_t j = 0; j < h->value_len; j++) {
                if (h->value[j] >= '0' && h->value[j] <= '9') {
                    req->content_length = req->content_length * 10 +
                                          (size_t)(h->value[j] - '0');
                }
            }
            // Reject requests with body larger than MAX_BODY_SIZE
            if (req->content_length > MAX_BODY_SIZE) {
                return HTTP_PARSE_ERROR;
            }
        }
        if (strncasecmp_eq(h->name, h->name_len, "connection", 10)) {
            if (strncasecmp_eq(h->value, h->value_len, "close", 5)) {
                req->keep_alive = false;
            } else if (strncasecmp_eq(h->value, h->value_len, "keep-alive", 10)) {
                req->keep_alive = true;
            }
        }
    }

    // Default keep-alive based on HTTP version
    bool found_connection = false;
    for (int i = 0; i < req->header_count; i++) {
        if (strncasecmp_eq(req->headers[i].name, req->headers[i].name_len,
                           "connection", 10)) {
            found_connection = true;
            break;
        }
    }
    if (!found_connection) {
        req->keep_alive = (req->version_major == 1 && req->version_minor == 1);
    }

    // Body
    if (req->content_length > 0) {
        size_t available = (size_t)(end - pos);
        if (available < req->content_length) {
            return HTTP_PARSE_INCOMPLETE;
        }
        req->body = pos;
        req->body_len = req->content_length;
        pos += req->content_length;
    } else {
        req->body = NULL;
        req->body_len = 0;
    }

    req->bytes_consumed = (size_t)(pos - buf);
    return HTTP_PARSE_OK;
}

const char* http_get_header(const HttpRequest* req, const char* name,
                            uint32_t* out_len) {
    uint32_t name_len = (uint32_t)strlen(name);
    for (int i = 0; i < req->header_count; i++) {
        if (strncasecmp_eq(req->headers[i].name, req->headers[i].name_len,
                           name, name_len)) {
            if (out_len) *out_len = req->headers[i].value_len;
            return req->headers[i].value;
        }
    }
    if (out_len) *out_len = 0;
    return NULL;
}
