#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

/*
 * KV stdlib package - purely in-memory key-value store.
 */

static ObjMap* kv_store = NULL;

static Value native_kv_set(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ObjString* key = AS_STRING(args[0]);
    Value value = args[1];
    obj_map_set(kv_store, key, value);
    return VAL_NIL;
}

static Value native_kv_get(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ObjString* key = AS_STRING(args[0]);
    Value value;
    if (obj_map_get(kv_store, key, &value)) return value;
    return VAL_NIL;
}

static Value native_kv_delete(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ObjString* key = AS_STRING(args[0]);
    obj_map_delete(kv_store, key);
    return VAL_NIL;
}

static Value native_kv_clear(int arg_count, Value* args) {
    (void)arg_count; (void)args;
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
    Value value;
    if (obj_map_get(kv_store, key, &value)) return VAL_TRUE;
    return VAL_FALSE;
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
}
