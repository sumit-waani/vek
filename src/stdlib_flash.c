#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"
#include <stdlib.h>
#include <string.h>

// Global flash store - a pinned ObjMap keyed by type string -> message string
static ObjMap* flash_store = NULL;

// flash.set(type, message) - Store a flash message
static Value native_flash_set(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;
    if (!IS_STRING(args[1])) return VAL_NIL;
    ObjString* type = AS_STRING(args[0]);
    Value message = args[1];
    obj_map_set(flash_store, type, message);
    return VAL_NIL;
}

// flash.get(type) - Retrieve and clear the flash of given type
static Value native_flash_get(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ObjString* key = AS_STRING(args[0]);
    Value value;
    if (obj_map_get(flash_store, key, &value)) {
        obj_map_delete(flash_store, key);
        return value;
    }
    return VAL_NIL;
}

// flash.has(type) - Returns true/false whether a flash of that type exists
static Value native_flash_has(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_FALSE;
    ObjString* key = AS_STRING(args[0]);
    Value value;
    if (obj_map_get(flash_store, key, &value)) {
        return VAL_TRUE;
    }
    return VAL_FALSE;
}

// flash.all() - Get all pending flash messages as a list of maps, then clear
static Value native_flash_all(int arg_count, Value* args) {
    (void)arg_count;
    (void)args;

    ObjList* list = obj_list_new();
    gc_push_root(OBJ_VAL(list));

    // Iterate over flash_store entries
    for (uint32_t i = 0; i < flash_store->capacity; i++) {
        MapEntry* entry = &flash_store->entries[i];
        if (entry->key == NULL || entry->key == MAP_TOMBSTONE) continue;

        ObjMap* item = obj_map_new();
        gc_push_root(OBJ_VAL(item));

        ObjString* type_key = obj_string_new("type", 4);
        obj_map_set(item, type_key, OBJ_VAL(entry->key));

        ObjString* msg_key = obj_string_new("message", 7);
        obj_map_set(item, msg_key, entry->value);

        obj_list_push(list, OBJ_VAL(item));
        gc_pop_root(); // item
    }

    gc_pop_root(); // list

    // Clear all flash messages
    ObjMap* new_store = obj_map_new();
    vm_pin((ObjHeader*)new_store);
    gc_track_object((ObjHeader*)new_store);
    vm_unpin((ObjHeader*)flash_store);
    flash_store = new_store;

    return OBJ_VAL(list);
}

// flash.clear() - Clear all flash messages without reading them
static Value native_flash_clear(int arg_count, Value* args) {
    (void)arg_count;
    (void)args;

    ObjMap* new_store = obj_map_new();
    vm_pin((ObjHeader*)new_store);
    gc_track_object((ObjHeader*)new_store);
    vm_unpin((ObjHeader*)flash_store);
    flash_store = new_store;
    return VAL_NIL;
}

// HTML escape helper: writes escaped version of src into dst buffer
// Returns number of bytes written
static size_t html_escape_into(char* dst, const char* src, uint32_t src_len) {
    size_t written = 0;
    for (uint32_t i = 0; i < src_len; i++) {
        char c = src[i];
        switch (c) {
            case '&':
                memcpy(dst + written, "&amp;", 5);
                written += 5;
                break;
            case '<':
                memcpy(dst + written, "&lt;", 4);
                written += 4;
                break;
            case '>':
                memcpy(dst + written, "&gt;", 4);
                written += 4;
                break;
            case '"':
                memcpy(dst + written, "&quot;", 6);
                written += 6;
                break;
            case '\'':
                memcpy(dst + written, "&#39;", 5);
                written += 5;
                break;
            default:
                dst[written] = c;
                written += 1;
                break;
        }
    }
    return written;
}

// Calculate escaped length
static size_t html_escaped_len(const char* src, uint32_t src_len) {
    size_t len = 0;
    for (uint32_t i = 0; i < src_len; i++) {
        char c = src[i];
        switch (c) {
            case '&':  len += 5; break;
            case '<':  len += 4; break;
            case '>':  len += 4; break;
            case '"':  len += 6; break;
            case '\'': len += 5; break;
            default:   len += 1; break;
        }
    }
    return len;
}

// flash.render_inline() - Returns HTML string for all pending flash messages
static Value native_flash_render_inline(int arg_count, Value* args) {
    (void)arg_count;
    (void)args;

    // Count entries to check if empty
    if (flash_store->length == 0) {
        ObjString* empty = obj_string_new("", 0);
        return OBJ_VAL(empty);
    }

    // Calculate total buffer size needed
    // Each entry: <div class="flash flash-{type}">{escaped_message}</div>
    // prefix = '<div class="flash flash-' (24 chars)
    // mid = '">' (2 chars)
    // suffix = '</div>' (6 chars)
    size_t total_len = 0;
    for (uint32_t i = 0; i < flash_store->capacity; i++) {
        MapEntry* entry = &flash_store->entries[i];
        if (entry->key == NULL || entry->key == MAP_TOMBSTONE) continue;
        if (!IS_STRING(entry->value)) continue;

        ObjString* type_str = entry->key;
        ObjString* msg_str = AS_STRING(entry->value);

        // <div class="flash flash-{type}">{escaped_message}</div>
        total_len += 24; // <div class="flash flash-
        total_len += type_str->length;
        total_len += 2;  // ">
        total_len += html_escaped_len(msg_str->data, msg_str->length);
        total_len += 6;  // </div>
    }

    // Allocate buffer
    char* buf = (char*)malloc(total_len + 1);
    if (!buf) return VAL_NIL;

    size_t offset = 0;
    for (uint32_t i = 0; i < flash_store->capacity; i++) {
        MapEntry* entry = &flash_store->entries[i];
        if (entry->key == NULL || entry->key == MAP_TOMBSTONE) continue;
        if (!IS_STRING(entry->value)) continue;

        ObjString* type_str = entry->key;
        ObjString* msg_str = AS_STRING(entry->value);

        // Write prefix
        memcpy(buf + offset, "<div class=\"flash flash-", 24);
        offset += 24;

        // Write type
        memcpy(buf + offset, type_str->data, type_str->length);
        offset += type_str->length;

        // Write mid
        memcpy(buf + offset, "\">", 2);
        offset += 2;

        // Write escaped message
        offset += html_escape_into(buf + offset, msg_str->data, msg_str->length);

        // Write suffix
        memcpy(buf + offset, "</div>", 6);
        offset += 6;
    }
    buf[offset] = '\0';

    ObjString* result = obj_string_new(buf, (uint32_t)offset);
    free(buf);

    // Clear all flash messages after rendering
    ObjMap* new_store = obj_map_new();
    vm_pin((ObjHeader*)new_store);
    gc_track_object((ObjHeader*)new_store);
    vm_unpin((ObjHeader*)flash_store);
    flash_store = new_store;

    return OBJ_VAL(result);
}

void stdlib_flash_init(ObjMap* pkg) {
    // Initialize the global flash store
    flash_store = obj_map_new();
    vm_pin((ObjHeader*)flash_store);
    gc_track_object((ObjHeader*)flash_store);

    stdlib_register(pkg, "set", native_flash_set, 2);
    stdlib_register(pkg, "get", native_flash_get, 1);
    stdlib_register(pkg, "has", native_flash_has, 1);
    stdlib_register(pkg, "all", native_flash_all, 0);
    stdlib_register(pkg, "clear", native_flash_clear, 0);
    stdlib_register(pkg, "render_inline", native_flash_render_inline, 0);
}
