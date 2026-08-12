#include "stdlib.h"
#include "vm.h"
#include "gc.h"

// Helper: register a native function into a package map
void stdlib_register(ObjMap* pkg, const char* name, NativeFn fn, int arity) {
    ObjNative* native = (ObjNative*)vek_alloc(sizeof(ObjNative));
    native->header.type = OBJ_NATIVE;
    native->header.flags = OBJ_FLAG_PIN;
    native->header.size = sizeof(ObjNative);
    native->header.hash = 0;
    native->header.page = NULL;
    native->function = fn;
    native->name = name;
    native->arity = arity;
    gc_track_object((ObjHeader*)native);

    ObjString* name_str = obj_string_new(name, (uint32_t)strlen(name));
    obj_map_set(pkg, name_str, OBJ_VAL(native));
}

// Create a package map and register it as a global
static ObjMap* create_package(const char* name) {
    ObjMap* pkg = obj_map_new();
    gc_push_root(OBJ_VAL(pkg));
    vm_pin((ObjHeader*)pkg);

    ObjString* name_str = obj_string_new(name, (uint32_t)strlen(name));
    obj_map_set(vm.globals, name_str, OBJ_VAL(pkg));

    gc_pop_root();
    return pkg;
}

void stdlib_init(void) {
    ObjMap* log_pkg = create_package("log");
    stdlib_log_init(log_pkg);

    ObjMap* env_pkg = create_package("env");
    stdlib_env_init(env_pkg);

    ObjMap* json_pkg = create_package("json");
    stdlib_json_init(json_pkg);

    ObjMap* time_pkg = create_package("time");
    stdlib_time_init(time_pkg);

    ObjMap* crypto_pkg = create_package("crypto");
    stdlib_crypto_init(crypto_pkg);
}
