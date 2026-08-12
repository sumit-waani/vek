#ifndef VEK_ROUTER_H
#define VEK_ROUTER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Maximum number of route parameters (e.g., /posts/:id/comments/:cid)
#define ROUTER_MAX_PARAMS 16
// Maximum children per node
#define ROUTER_MAX_CHILDREN 32
// Maximum path segment length
#define ROUTER_MAX_SEGMENT 128

// HTTP methods for routing (maps to HttpMethod enum)
#define ROUTE_METHOD_COUNT 7

// Matched route parameters
typedef struct {
    const char* name;     // parameter name (e.g., "id")
    const char* value;    // matched value
    uint32_t    name_len;
    uint32_t    value_len;
} RouteParam;

typedef struct {
    RouteParam params[ROUTER_MAX_PARAMS];
    int        count;
} RouteParams;

// Router trie node
typedef struct RouterNode {
    char     segment[ROUTER_MAX_SEGMENT]; // segment text (without leading '/')
    bool     is_param;                    // true if this is a :param node
    void*    handlers[ROUTE_METHOD_COUNT]; // handler per HTTP method (NULL = no handler)

    struct RouterNode* children[ROUTER_MAX_CHILDREN];
    int child_count;
} RouterNode;

// Router
typedef struct {
    RouterNode* root;
} Router;

// Initialize a router
void router_init(Router* router);

// Free all router memory
void router_destroy(Router* router);

// Add a route. method is the HTTP method index (0=GET, 1=POST, etc.)
// path is like "/posts/:id/comments"
// handler is user data to associate with this route+method
void router_add(Router* router, int method, const char* path, void* handler);

// Match a path against the router.
// Returns the handler for the given method, or NULL if no match.
// Fills params with any captured dynamic segments.
void* router_match(Router* router, int method, const char* path,
                   uint32_t path_len, RouteParams* params);

#endif // VEK_ROUTER_H
