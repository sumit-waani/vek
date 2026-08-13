#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"
#include "redis_client.h"

/*
 * KV stdlib package - in-memory by default, optional Redis backend.
 *
 * Call kv.use_redis() to switch to Redis backend when REDIS_URL is set.
 * If REDIS_URL is not present, kv.use_redis() is a no-op and kv stays in-memory.
 */

static ObjMap* kv_store = NULL;
static redis_conn* kv_redis = NULL;

static Value native_kv_set(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ObjString* key = AS_STRING(args[0]);

    if (kv_redis) {
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
        redis_set(kv_redis, key->data, val_str, val_len);
        return VAL_NIL;
    }

    Value value = args[1];
    obj_map_set(kv_store, key, value);
    return VAL_NIL;
}

static Value native_kv_get(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ObjString* key = AS_STRING(args[0]);

    if (kv_redis) {
        char* value = NULL;
        int value_len = 0;
        int rc = redis_get(kv_redis, key->data, &value, &value_len);
        if (rc != REDIS_OK || !value) return VAL_NIL;
        ObjString* result = obj_string_new(value, (uint32_t)value_len);
        free(value);
        return OBJ_VAL(result);
    }

    Value value;
    if (obj_map_get(kv_store, key, &value)) return value;
    return VAL_NIL;
}

static Value native_kv_delete(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ObjString* key = AS_STRING(args[0]);

    if (kv_redis) {
        redis_del(kv_redis, key->data);
        return VAL_NIL;
    }

    obj_map_delete(kv_store, key);
    return VAL_NIL;
}

static Value native_kv_clear(int arg_count, Value* args) {
    (void)arg_count; (void)args;

    if (kv_redis) return VAL_NIL;

    ObjMap* new_store = obj_map_new();
    vm_pin((ObjHeader*)new_store);
    gc_track_object((ObjHeader*)new_store);
    vm_unpin((ObjHeader*)kv_store);
    kv_store = new_store;
    return VAL_NIL;
}

static Value native_kv_has(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_FALSE;
    ObjString* key = AS_STRING(args[0]);

    if (kv_redis) {
        char* value = NULL;
        int value_len = 0;
        int rc = redis_get(kv_redis, key->data, &value, &value_len);
        if (value) free(value);
        return (rc == REDIS_OK) ? VAL_TRUE : VAL_FALSE;
    }

    Value value;
    if (obj_map_get(kv_store, key, &value)) return VAL_TRUE;
    return VAL_FALSE;
}

static Value native_kv_use_redis(int arg_count, Value* args) {
    (void)arg_count; (void)args;
    if (kv_redis) return VAL_TRUE;
    kv_redis = redis_connect();
    if (!kv_redis) return VAL_NIL;
    return VAL_TRUE;
}

void stdlib_kv_init(ObjMap* pkg) {
    kv_store = obj_map_new();
    vm_pin((ObjHeader*)kv_store);
    gc_track_object((ObjHeader*)kv_store);

    stdlib_register(pkg, "set", native_kv_set, 2);
    stdlib_register(pkg, "get", native_kv_get, 1);
    stdlib_register(pkg, "delete", native_kv_delete, 1);
    stdlib_register(pkg, "clear", native_kv_clear, 0);
    stdlib_register(pkg, "has", native_kv_has, 1);
    stdlib_register(pkg, "use_redis", native_kv_use_redis, 0);
}
