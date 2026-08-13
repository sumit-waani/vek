#define _POSIX_C_SOURCE 200809L
#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"
#include "redis_client.h"

#include <time.h>

/*
 * Cache stdlib package - in-memory TTL cache by default, optional Redis backend.
 *
 * Call cache.use_redis() to switch to Redis backend when REDIS_URL is set.
 */

static ObjMap* cache_store = NULL;
static redis_conn* cache_redis = NULL;

static Value native_cache_set(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;
    if (!IS_INT(args[2]) && !IS_FLOAT(args[2])) return VAL_NIL;

    ObjString* key = AS_STRING(args[0]);
    int64_t ttl;
    if (IS_INT(args[2])) { ttl = AS_INT(args[2]); }
    else { ttl = (int64_t)AS_DOUBLE(args[2]); }

    if (cache_redis) {
        const char* val_str = "";
        int val_len = 0;
        char num_buf[64];

        if (IS_STRING(args[1])) {
            val_str = AS_STRING(args[1])->data;
            val_len = (int)AS_STRING(args[1])->length;
        } else if (IS_INT(args[1])) {
            val_len = snprintf(num_buf, sizeof(num_buf), "%lld", (long long)AS_INT(args[1]));
            val_str = num_buf;
        } else if (IS_FLOAT(args[1])) {
            val_len = snprintf(num_buf, sizeof(num_buf), "%g", AS_DOUBLE(args[1]));
            val_str = num_buf;
        } else if (IS_BOOL(args[1])) {
            val_str = AS_BOOL(args[1]) ? "true" : "false";
            val_len = AS_BOOL(args[1]) ? 4 : 5;
        }
        redis_set(cache_redis, key->data, val_str, val_len);
        redis_expire(cache_redis, key->data, ttl);
        return VAL_NIL;
    }

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

    if (cache_redis) {
        char* value = NULL;
        int value_len = 0;
        int rc = redis_get(cache_redis, key->data, &value, &value_len);
        if (rc != REDIS_OK || !value) return VAL_NIL;
        ObjString* result = obj_string_new(value, (uint32_t)value_len);
        free(value);
        return OBJ_VAL(result);
    }

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

    if (cache_redis) {
        redis_del(cache_redis, key->data);
        return VAL_NIL;
    }

    obj_map_delete(cache_store, key);
    return VAL_NIL;
}

static Value native_cache_use_redis(int arg_count, Value* args) {
    (void)arg_count; (void)args;
    if (cache_redis) return VAL_TRUE;
    cache_redis = redis_connect();
    if (!cache_redis) return VAL_NIL;
    return VAL_TRUE;
}

void stdlib_cache_init(ObjMap* pkg) {
    cache_store = obj_map_new();
    vm_pin((ObjHeader*)cache_store);
    gc_track_object((ObjHeader*)cache_store);

    stdlib_register(pkg, "set", native_cache_set, 3);
    stdlib_register(pkg, "get", native_cache_get, 1);
    stdlib_register(pkg, "delete", native_cache_delete, 1);
    stdlib_register(pkg, "use_redis", native_cache_use_redis, 0);
}
