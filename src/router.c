/*
 * router.c - Trie-based URL router with dynamic segment support.
 * Supports literal segments and :param segments.
 */

#include "router.h"
#include <stdlib.h>
#include <string.h>

static RouterNode* node_new(const char* segment, uint32_t seg_len, bool is_param) {
    RouterNode* node = calloc(1, sizeof(RouterNode));
    if (!node) return NULL;

    uint32_t copy_len = seg_len < ROUTER_MAX_SEGMENT - 1 ? seg_len : ROUTER_MAX_SEGMENT - 1;
    memcpy(node->segment, segment, copy_len);
    node->segment[copy_len] = '\0';
    node->is_param = is_param;
    node->child_count = 0;

    for (int i = 0; i < ROUTE_METHOD_COUNT; i++) {
        node->handlers[i] = NULL;
    }
    return node;
}

static void node_free(RouterNode* node) {
    if (!node) return;
    for (int i = 0; i < node->child_count; i++) {
        node_free(node->children[i]);
    }
    free(node);
}

void router_init(Router* router) {
    router->root = node_new("", 0, false);
}

void router_destroy(Router* router) {
    if (router->root) {
        node_free(router->root);
        router->root = NULL;
    }
}

// Find or create a child node for the given segment
static RouterNode* find_or_create_child(RouterNode* parent,
                                        const char* segment, uint32_t seg_len,
                                        bool is_param) {
    // Search existing children
    for (int i = 0; i < parent->child_count; i++) {
        RouterNode* child = parent->children[i];
        if (child->is_param == is_param) {
            uint32_t clen = (uint32_t)strlen(child->segment);
            if (clen == seg_len && memcmp(child->segment, segment, seg_len) == 0) {
                return child;
            }
        }
    }

    // Create new child
    if (parent->child_count >= ROUTER_MAX_CHILDREN) return NULL;

    RouterNode* child = node_new(segment, seg_len, is_param);
    if (!child) return NULL;

    parent->children[parent->child_count++] = child;
    return child;
}

void router_add(Router* router, int method, const char* path, void* handler) {
    if (!router->root || method < 0 || method >= ROUTE_METHOD_COUNT) return;

    RouterNode* current = router->root;

    // Skip leading slash
    const char* p = path;
    if (*p == '/') p++;

    // Handle root path
    if (*p == '\0') {
        current->handlers[method] = handler;
        return;
    }

    // Split path into segments
    while (*p) {
        const char* seg_start = p;
        while (*p && *p != '/') p++;
        uint32_t seg_len = (uint32_t)(p - seg_start);

        bool is_param = (seg_start[0] == ':');
        const char* actual_seg = seg_start;
        uint32_t actual_len = seg_len;

        if (is_param) {
            // Store just the param name (without the colon)
            actual_seg = seg_start + 1;
            actual_len = seg_len - 1;
        }

        current = find_or_create_child(current, actual_seg, actual_len, is_param);
        if (!current) return;

        if (*p == '/') p++;
    }

    current->handlers[method] = handler;
}

// Recursive matching helper
static void* match_recursive(RouterNode* node, const char* path, uint32_t path_len,
                             int method, RouteParams* params) {
    // Skip leading slash
    if (path_len > 0 && path[0] == '/') {
        path++;
        path_len--;
    }

    // If path is empty, check this node for a handler
    if (path_len == 0) {
        return node->handlers[method];
    }

    // Extract next segment
    const char* seg_start = path;
    uint32_t seg_len = 0;
    while (seg_len < path_len && path[seg_len] != '/') {
        seg_len++;
    }

    const char* remaining = path + seg_len;
    uint32_t remaining_len = path_len - seg_len;

    // Try literal children first
    for (int i = 0; i < node->child_count; i++) {
        RouterNode* child = node->children[i];
        if (!child->is_param) {
            uint32_t clen = (uint32_t)strlen(child->segment);
            if (clen == seg_len && memcmp(child->segment, seg_start, seg_len) == 0) {
                void* result = match_recursive(child, remaining, remaining_len,
                                               method, params);
                if (result) return result;
            }
        }
    }

    // Try param children
    for (int i = 0; i < node->child_count; i++) {
        RouterNode* child = node->children[i];
        if (child->is_param && params->count < ROUTER_MAX_PARAMS) {
            int saved_count = params->count;

            // Capture this segment as the param value
            RouteParam* param = &params->params[params->count++];
            param->name = child->segment;
            param->name_len = (uint32_t)strlen(child->segment);
            param->value = seg_start;
            param->value_len = seg_len;

            void* result = match_recursive(child, remaining, remaining_len,
                                           method, params);
            if (result) return result;

            // Backtrack
            params->count = saved_count;
        }
    }

    return NULL;
}

void* router_match(Router* router, int method, const char* path,
                   uint32_t path_len, RouteParams* params) {
    if (!router->root || method < 0 || method >= ROUTE_METHOD_COUNT) return NULL;
    params->count = 0;
    return match_recursive(router->root, path, path_len, method, params);
}
