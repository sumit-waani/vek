#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"
#include "redis_client.h"

/*
 * Redis stdlib package - exposes Redis operations to vek scripts.
 *
 * Provides: redis.connect, redis.get, redis.set, redis.del,
 *           redis.incr, redis.expire, redis.publish, redis.subscribe
 */

static redis_conn* redis_connection = NULL;

static Value native_redis_connect(int argc, Value* args) {
    (void)argc; (void)args;
    if (redis_connection) {
        redis_disconnect(redis_connection);
        redis_connection = NULL;
    }
    redis_connection = redis_connect();
    if (!redis_connection) return VAL_NIL;
    return VAL_TRUE;
}

static Value native_redis_get(int argc, Value* args) {
    (void)argc;
    if (!redis_connection) return VAL_NIL;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ObjString* key = AS_STRING(args[0]);
    char* value = NULL;
    int value_len = 0;
    int rc = redis_get(redis_connection, key->data, &value, &value_len);
    if (rc != REDIS_OK || !value) return VAL_NIL;
    ObjString* result = obj_string_new(value, (uint32_t)value_len);
    free(value);
    return OBJ_VAL(result);
}

static Value native_redis_set(int argc, Value* args) {
    (void)argc;
    if (!redis_connection) return VAL_NIL;
    if (!IS_STRING(args[0]) || !IS_STRING(args[1])) return VAL_NIL;
    ObjString* key = AS_STRING(args[0]);
    ObjString* value = AS_STRING(args[1]);
    int rc = redis_set(redis_connection, key->data, value->data, (int)value->length);
    if (rc != REDIS_OK) return VAL_NIL;
    return VAL_TRUE;
}

static Value native_redis_del(int argc, Value* args) {
    (void)argc;
    if (!redis_connection) return VAL_NIL;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ObjString* key = AS_STRING(args[0]);
    int rc = redis_del(redis_connection, key->data);
    if (rc != REDIS_OK) return VAL_NIL;
    return VAL_TRUE;
}

static Value native_redis_incr(int argc, Value* args) {
    (void)argc;
    if (!redis_connection) return VAL_NIL;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ObjString* key = AS_STRING(args[0]);
    int64_t new_value = 0;
    int rc = redis_incr(redis_connection, key->data, &new_value);
    if (rc != REDIS_OK) return VAL_NIL;
    return INT_VAL(new_value);
}

static Value native_redis_expire(int argc, Value* args) {
    (void)argc;
    if (!redis_connection) return VAL_NIL;
    if (!IS_STRING(args[0])) return VAL_NIL;
    if (!IS_INT(args[1])) return VAL_NIL;
    ObjString* key = AS_STRING(args[0]);
    int64_t seconds = AS_INT(args[1]);
    int rc = redis_expire(redis_connection, key->data, seconds);
    if (rc != REDIS_OK) return VAL_NIL;
    return VAL_TRUE;
}

static Value native_redis_publish(int argc, Value* args) {
    (void)argc;
    if (!redis_connection) return VAL_NIL;
    if (!IS_STRING(args[0]) || !IS_STRING(args[1])) return VAL_NIL;
    ObjString* channel = AS_STRING(args[0]);
    ObjString* message = AS_STRING(args[1]);
    int rc = redis_publish(redis_connection, channel->data, message->data, (int)message->length);
    if (rc != REDIS_OK) return VAL_NIL;
    return VAL_TRUE;
}

static Value native_redis_subscribe(int argc, Value* args) {
    (void)argc;
    if (!redis_connection) return VAL_NIL;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ObjString* channel = AS_STRING(args[0]);
    int rc = redis_subscribe(redis_connection, channel->data);
    if (rc != REDIS_OK) return VAL_NIL;
    return VAL_TRUE;
}

void stdlib_redis_init(ObjMap* pkg) {
    stdlib_register(pkg, "connect", native_redis_connect, 0);
    stdlib_register(pkg, "get", native_redis_get, 1);
    stdlib_register(pkg, "set", native_redis_set, 2);
    stdlib_register(pkg, "del", native_redis_del, 1);
    stdlib_register(pkg, "incr", native_redis_incr, 1);
    stdlib_register(pkg, "expire", native_redis_expire, 2);
    stdlib_register(pkg, "publish", native_redis_publish, 2);
    stdlib_register(pkg, "subscribe", native_redis_subscribe, 1);
}
