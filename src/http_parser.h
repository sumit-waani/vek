#ifndef VEK_HTTP_PARSER_H
#define VEK_HTTP_PARSER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// HTTP methods
typedef enum {
    HTTP_GET,
    HTTP_POST,
    HTTP_PUT,
    HTTP_PATCH,
    HTTP_DELETE,
    HTTP_HEAD,
    HTTP_OPTIONS,
    HTTP_METHOD_UNKNOWN
} HttpMethod;

// Parse result codes
typedef enum {
    HTTP_PARSE_OK,
    HTTP_PARSE_INCOMPLETE,
    HTTP_PARSE_ERROR
} HttpParseResult;

// Maximum number of headers
#define HTTP_MAX_HEADERS 64

// A single header (zero-copy: pointers into the buffer)
typedef struct {
    const char* name;
    uint32_t    name_len;
    const char* value;
    uint32_t    value_len;
} HttpHeader;

// Parsed HTTP request (zero-copy: pointers into the buffer)
typedef struct {
    HttpMethod   method;
    const char*  method_str;
    uint32_t     method_len;

    const char*  path;
    uint32_t     path_len;

    const char*  query_string;   // NULL if no query string
    uint32_t     query_len;

    int          version_major;  // 1
    int          version_minor;  // 0 or 1

    HttpHeader   headers[HTTP_MAX_HEADERS];
    int          header_count;

    const char*  body;
    size_t       body_len;
    size_t       content_length;

    bool         keep_alive;

    // Total bytes consumed from buffer (for next request)
    size_t       bytes_consumed;
} HttpRequest;

// Parse an HTTP request from a buffer.
// Returns HTTP_PARSE_OK on success, HTTP_PARSE_INCOMPLETE if not enough data,
// HTTP_PARSE_ERROR on malformed request.
// On success, req is filled in with pointers into buf.
HttpParseResult http_parse_request(const char* buf, size_t len, HttpRequest* req);

// Get the string representation of an HTTP method
const char* http_method_str(HttpMethod method);

// Get a header value by name (case-insensitive). Returns NULL if not found.
const char* http_get_header(const HttpRequest* req, const char* name, uint32_t* out_len);

#endif // VEK_HTTP_PARSER_H
