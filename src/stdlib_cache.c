#define _POSIX_C_SOURCE 200809L
#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

#include <time.h>

/*
 * Cache stdlib package - purely in-memory TTL cache.
 */

static ObjMap* cache_store = NULL;

static Value native_cache_set(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;
    if (!IS_INT(args[2]) && !IS_FLOAT(args[2])) return VAL_NIL;

    ObjString* key = AS_STRING(args[0]);
    int64_t ttl;
    if (IS_INT(args[2])) { ttl = AS_INT(args[2]); }
    else { ttl = (int64_t)AS_DOUBLE(args[2]); }

    Value value = args[1];
    time_t now = time(NULL);
    int64_t expires_at = (int64_t)now + ttl;

    ObjMap* entry = obj_map_new();
    gc_push_root(OBJ_VAL(entry));

    ObjString* v_key = obj_string_new("v", 1);
    obj_map_set(entry, v_key, value);

    ObjString* e_key = obj_string_new("e", 1);
    obj_map_set(entry, e_key, INT_VAL(expires_at));

    obj_map_set(cache_store, key, OBJ_VAL(entry));
    gc_pop_root();
    return VAL_NIL;
}

static Value native_cache_get(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ObjString* key = AS_STRING(args[0]);

    Value entry_val;
    if (!obj_map_get(cache_store, key, &entry_val)) return VAL_NIL;
    if (!IS_MAP(entry_val)) return VAL_NIL;
    ObjMap* entry = AS_MAP(entry_val);

    ObjString* e_key = obj_string_new("e", 1);
    Value expires_val;
    if (!obj_map_get(entry, e_key, &expires_val)) return VAL_NIL;
    if (!IS_INT(expires_val)) return VAL_NIL;

    int64_t expires_at = AS_INT(expires_val);
    time_t now = time(NULL);
    if ((int64_t)now >= expires_at) {
        obj_map_delete(cache_store, key);
        return VAL_NIL;
    }

    ObjString* v_key = obj_string_new("v", 1);
    Value value;
    if (!obj_map_get(entry, v_key, &value)) return VAL_NIL;
    return value;
}

static Value native_cache_delete(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ObjString* key = AS_STRING(args[0]);
    obj_map_delete(cache_store, key);
    return VAL_NIL;
}

void stdlib_cache_init(ObjMap* pkg) {
    cache_store = obj_map_new();
    vm_pin((ObjHeader*)cache_store);
    gc_track_object((ObjHeader*)cache_store);

    stdlib_register(pkg, "set", native_cache_set, 3);
    stdlib_register(pkg, "get", native_cache_get, 1);
    stdlib_register(pkg, "delete", native_cache_delete, 1);
}
