#include "vek_stdlib.h"
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

    ObjMap* server_pkg = create_package("server");
    stdlib_http_server_init(server_pkg);

    ObjMap* path_pkg = create_package("path");
    stdlib_path_init(path_pkg);

    ObjMap* uuid_pkg = create_package("uuid");
    stdlib_uuid_init(uuid_pkg);

    ObjMap* kv_pkg = create_package("kv");
    stdlib_kv_init(kv_pkg);

    ObjMap* cache_pkg = create_package("cache");
    stdlib_cache_init(cache_pkg);

    ObjMap* db_pkg = create_package("db");
    stdlib_db_init(db_pkg);

    ObjMap* html_pkg = create_package("html");
    stdlib_view_init(html_pkg);

    ObjMap* session_pkg = create_package("session");
    stdlib_session_init(session_pkg);

    ObjMap* csrf_pkg = create_package("csrf");
    stdlib_csrf_init(csrf_pkg);

    ObjMap* form_pkg = create_package("form");
    stdlib_form_init(form_pkg);

    ObjMap* pages_pkg = create_package("pages");
    stdlib_pages_init(pages_pkg);

    ObjMap* auth_pkg = create_package("auth");
    stdlib_auth_init(auth_pkg);

    ObjMap* slug_pkg = create_package("slug");
    stdlib_slug_init(slug_pkg);

    ObjMap* csp_pkg = create_package("csp");
    stdlib_csp_init(csp_pkg);

    ObjMap* cors_pkg = create_package("cors");
    stdlib_cors_init(cors_pkg);

    ObjMap* i18n_pkg = create_package("i18n");
    stdlib_i18n_init(i18n_pkg);

    ObjMap* ratelimit_pkg = create_package("ratelimit");
    stdlib_ratelimit_init(ratelimit_pkg);

    ObjMap* markdown_pkg = create_package("markdown");
    stdlib_markdown_init(markdown_pkg);

    ObjMap* sanitize_pkg = create_package("sanitize");
    stdlib_sanitize_init(sanitize_pkg);

    ObjMap* webhook_pkg = create_package("webhook");
    stdlib_webhook_init(webhook_pkg);

    ObjMap* cli_pkg = create_package("cli");
    stdlib_cli_init(cli_pkg);

    ObjMap* compress_pkg = create_package("compress");
    stdlib_compress_init(compress_pkg);

    ObjMap* http_pkg = create_package("http");
    stdlib_http_client_init(http_pkg);

    ObjMap* mail_pkg = create_package("mail");
    stdlib_mail_init(mail_pkg);

    ObjMap* jobs_pkg = create_package("jobs");
    stdlib_jobs_init(jobs_pkg);

    ObjMap* storage_pkg = create_package("storage");
    stdlib_storage_init(storage_pkg);

    ObjMap* ws_pkg = create_package("ws");
    stdlib_websocket_init(ws_pkg);
}
