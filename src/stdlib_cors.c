#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

// Helper: get string value from map by key
static ObjString* cors_map_get_str(ObjMap* map, const char* key) {
    ObjString* k = obj_string_new(key, (uint32_t)strlen(key));
    Value val;
    if (obj_map_get(map, k, &val) && IS_STRING(val)) {
        return AS_STRING(val);
    }
    return NULL;
}

// Helper: get bool value from map by key
static bool cors_map_get_bool(ObjMap* map, const char* key) {
    ObjString* k = obj_string_new(key, (uint32_t)strlen(key));
    Value val;
    if (obj_map_get(map, k, &val)) {
        return val == VAL_TRUE;
    }
    return false;
}

// cors.headers(config_map) - returns a map of CORS headers
// config keys: origin, methods, headers, credentials, max_age
static Value native_cors_headers(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_MAP(args[0])) return VAL_NIL;
    ObjMap* config = AS_MAP(args[0]);

    ObjMap* result = obj_map_new();
    gc_push_root(OBJ_VAL(result));

    // Access-Control-Allow-Origin
    ObjString* origin = cors_map_get_str(config, "origin");
    if (origin) {
        ObjString* hdr_key = obj_string_new("Access-Control-Allow-Origin", 27);
        obj_map_set(result, hdr_key, OBJ_VAL(origin));
    } else {
        ObjString* hdr_key = obj_string_new("Access-Control-Allow-Origin", 27);
        ObjString* star = obj_string_new("*", 1);
        obj_map_set(result, hdr_key, OBJ_VAL(star));
    }

    // Access-Control-Allow-Methods
    ObjString* methods = cors_map_get_str(config, "methods");
    if (methods) {
        ObjString* hdr_key = obj_string_new("Access-Control-Allow-Methods", 28);
        obj_map_set(result, hdr_key, OBJ_VAL(methods));
    }

    // Access-Control-Allow-Headers
    ObjString* headers = cors_map_get_str(config, "headers");
    if (headers) {
        ObjString* hdr_key = obj_string_new("Access-Control-Allow-Headers", 28);
        obj_map_set(result, hdr_key, OBJ_VAL(headers));
    }

    // Access-Control-Allow-Credentials
    bool credentials = cors_map_get_bool(config, "credentials");
    if (credentials) {
        ObjString* hdr_key = obj_string_new("Access-Control-Allow-Credentials", 32);
        ObjString* val = obj_string_new("true", 4);
        obj_map_set(result, hdr_key, OBJ_VAL(val));
    }

    // Access-Control-Max-Age
    ObjString* max_age = cors_map_get_str(config, "max_age");
    if (max_age) {
        ObjString* hdr_key = obj_string_new("Access-Control-Max-Age", 22);
        obj_map_set(result, hdr_key, OBJ_VAL(max_age));
    }

    gc_pop_root();
    return OBJ_VAL(result);
}

// cors.preflight(request_map, config_map) - returns preflight response headers
// request_map may contain: method, origin, request_headers
// config_map: same as cors.headers
static Value native_cors_preflight(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_MAP(args[0]) || !IS_MAP(args[1])) return VAL_NIL;
    ObjMap* config = AS_MAP(args[1]);

    ObjMap* result = obj_map_new();
    gc_push_root(OBJ_VAL(result));

    // Access-Control-Allow-Origin
    ObjString* origin = cors_map_get_str(config, "origin");
    {
        ObjString* hdr_key = obj_string_new("Access-Control-Allow-Origin", 27);
        if (origin) {
            obj_map_set(result, hdr_key, OBJ_VAL(origin));
        } else {
            ObjString* star = obj_string_new("*", 1);
            obj_map_set(result, hdr_key, OBJ_VAL(star));
        }
    }

    // Access-Control-Allow-Methods
    ObjString* methods = cors_map_get_str(config, "methods");
    if (methods) {
        ObjString* hdr_key = obj_string_new("Access-Control-Allow-Methods", 28);
        obj_map_set(result, hdr_key, OBJ_VAL(methods));
    } else {
        ObjString* hdr_key = obj_string_new("Access-Control-Allow-Methods", 28);
        ObjString* default_methods = obj_string_new("GET, POST, PUT, DELETE, OPTIONS", 31);
        obj_map_set(result, hdr_key, OBJ_VAL(default_methods));
    }

    // Access-Control-Allow-Headers
    ObjString* headers = cors_map_get_str(config, "headers");
    if (headers) {
        ObjString* hdr_key = obj_string_new("Access-Control-Allow-Headers", 28);
        obj_map_set(result, hdr_key, OBJ_VAL(headers));
    } else {
        ObjString* hdr_key = obj_string_new("Access-Control-Allow-Headers", 28);
        ObjString* default_headers = obj_string_new("Content-Type, Authorization", 27);
        obj_map_set(result, hdr_key, OBJ_VAL(default_headers));
    }

    // Access-Control-Allow-Credentials
    bool credentials = cors_map_get_bool(config, "credentials");
    if (credentials) {
        ObjString* hdr_key = obj_string_new("Access-Control-Allow-Credentials", 32);
        ObjString* val = obj_string_new("true", 4);
        obj_map_set(result, hdr_key, OBJ_VAL(val));
    }

    // Access-Control-Max-Age
    ObjString* max_age = cors_map_get_str(config, "max_age");
    if (max_age) {
        ObjString* hdr_key = obj_string_new("Access-Control-Max-Age", 22);
        obj_map_set(result, hdr_key, OBJ_VAL(max_age));
    } else {
        ObjString* hdr_key = obj_string_new("Access-Control-Max-Age", 22);
        ObjString* default_age = obj_string_new("86400", 5);
        obj_map_set(result, hdr_key, OBJ_VAL(default_age));
    }

    gc_pop_root();
    return OBJ_VAL(result);
}

void stdlib_cors_init(ObjMap* pkg) {
    stdlib_register(pkg, "headers", native_cors_headers, 1);
    stdlib_register(pkg, "preflight", native_cors_preflight, 2);
}
