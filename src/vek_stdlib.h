#ifndef VEK_STDLIB_H
#define VEK_STDLIB_H

#include "common.h"
#include "value.h"
#include "object.h"
#include "vm.h"

// Initialize all stdlib packages (call after vm_init)
void stdlib_init(void);

// Per-package initialization functions
void stdlib_log_init(ObjMap* pkg);
void stdlib_env_init(ObjMap* pkg);
void stdlib_json_init(ObjMap* pkg);
void stdlib_time_init(ObjMap* pkg);
void stdlib_crypto_init(ObjMap* pkg);
void stdlib_http_server_init(ObjMap* pkg);
void stdlib_path_init(ObjMap* pkg);
void stdlib_uuid_init(ObjMap* pkg);
void stdlib_kv_init(ObjMap* pkg);
void stdlib_cache_init(ObjMap* pkg);
void stdlib_db_init(ObjMap* pkg);
void stdlib_view_init(ObjMap* pkg);
void stdlib_session_init(ObjMap* pkg);
void stdlib_csrf_init(ObjMap* pkg);
void stdlib_form_init(ObjMap* pkg);

// Helper: register a native function into a package map
void stdlib_register(ObjMap* pkg, const char* name, NativeFn fn, int arity);

#endif // VEK_STDLIB_H
