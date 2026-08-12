#define _POSIX_C_SOURCE 200809L
#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

#include <time.h>

// Helper: print a log message with timestamp and level to stderr
static void log_message(const char* level, int arg_count, Value* args) {
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

    fprintf(stderr, "[%s] %s: ", time_str, level);
    for (int i = 0; i < arg_count; i++) {
        if (i > 0) fprintf(stderr, " ");
        if (IS_STRING(args[i])) {
            ObjString* s = AS_STRING(args[i]);
            fprintf(stderr, "%.*s", (int)s->length, s->data);
        } else if (IS_NIL(args[i])) {
            fprintf(stderr, "nil");
        } else if (IS_BOOL(args[i])) {
            fprintf(stderr, "%s", AS_BOOL(args[i]) ? "true" : "false");
        } else if (IS_INT(args[i])) {
            fprintf(stderr, "%lld", (long long)AS_INT(args[i]));
        } else if (IS_FLOAT(args[i])) {
            fprintf(stderr, "%g", AS_DOUBLE(args[i]));
        } else {
            fprintf(stderr, "<object>");
        }
    }
    fprintf(stderr, "\n");
}

static Value native_log_info(int arg_count, Value* args) {
    log_message("INFO", arg_count, args);
    return VAL_NIL;
}

static Value native_log_warn(int arg_count, Value* args) {
    log_message("WARN", arg_count, args);
    return VAL_NIL;
}

static Value native_log_error(int arg_count, Value* args) {
    log_message("ERROR", arg_count, args);
    return VAL_NIL;
}

static Value native_log_debug(int arg_count, Value* args) {
    log_message("DEBUG", arg_count, args);
    return VAL_NIL;
}

void stdlib_log_init(ObjMap* pkg) {
    stdlib_register(pkg, "info", native_log_info, -1);
    stdlib_register(pkg, "warn", native_log_warn, -1);
    stdlib_register(pkg, "error", native_log_error, -1);
    stdlib_register(pkg, "debug", native_log_debug, -1);
}
