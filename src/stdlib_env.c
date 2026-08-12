#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

#include <stdlib.h>

// env.get(key) or env.get(key, default) - returns env var value or default/nil
static Value native_env_get(int arg_count, Value* args) {
    if (arg_count < 1 || !IS_STRING(args[0])) {
        return VAL_NIL;
    }
    ObjString* key = AS_STRING(args[0]);

    // Null-terminate the key for getenv (ObjString data is already NUL-terminated)
    const char* val = getenv(key->data);
    if (val != NULL) {
        return OBJ_VAL(obj_string_new(val, (uint32_t)strlen(val)));
    }

    // Return default if provided, otherwise nil
    if (arg_count >= 2) {
        return args[1];
    }
    return VAL_NIL;
}

// env.required(key) - returns env var or prints error and exits
static Value native_env_required(int arg_count, Value* args) {
    if (arg_count < 1 || !IS_STRING(args[0])) {
        fprintf(stderr, "env.required: expected string argument\n");
        exit(1);
    }
    ObjString* key = AS_STRING(args[0]);

    const char* val = getenv(key->data);
    if (val == NULL) {
        fprintf(stderr, "env.required: environment variable '%.*s' is not set\n",
                (int)key->length, key->data);
        exit(1);
    }
    return OBJ_VAL(obj_string_new(val, (uint32_t)strlen(val)));
}

void stdlib_env_init(ObjMap* pkg) {
    stdlib_register(pkg, "get", native_env_get, -1);
    stdlib_register(pkg, "required", native_env_required, 1);
}
