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
void stdlib_pages_init(ObjMap* pkg);
void stdlib_auth_init(ObjMap* pkg);
void stdlib_slug_init(ObjMap* pkg);
void stdlib_csp_init(ObjMap* pkg);
void stdlib_cors_init(ObjMap* pkg);
void stdlib_i18n_init(ObjMap* pkg);
void stdlib_ratelimit_init(ObjMap* pkg);
void stdlib_markdown_init(ObjMap* pkg);
void stdlib_sanitize_init(ObjMap* pkg);
void stdlib_webhook_init(ObjMap* pkg);
void stdlib_cli_init(ObjMap* pkg);
void stdlib_compress_init(ObjMap* pkg);
void stdlib_http_client_init(ObjMap* pkg);
void stdlib_mail_init(ObjMap* pkg);
void stdlib_jobs_init(ObjMap* pkg);
void stdlib_storage_init(ObjMap* pkg);
void stdlib_websocket_init(ObjMap* pkg);

// cli.c needs this called from main to store argc/argv
void cli_set_args(int argc, char** argv);

// Helper: register a native function into a package map
void stdlib_register(ObjMap* pkg, const char* name, NativeFn fn, int arity);

#endif // VEK_STDLIB_H
