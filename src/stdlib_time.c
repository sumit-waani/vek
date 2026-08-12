#define _POSIX_C_SOURCE 200809L
#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

#include <time.h>
#include <sys/time.h>

// time.now() - returns epoch seconds as integer
static Value native_time_now(int arg_count, Value* args) {
    (void)arg_count;
    (void)args;
    time_t now = time(NULL);
    return INT_VAL((int64_t)now);
}

// time.ms() - returns epoch milliseconds as integer
static Value native_time_ms(int arg_count, Value* args) {
    (void)arg_count;
    (void)args;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t ms = (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;
    return INT_VAL(ms);
}

// time.format(epoch, fmt) - returns formatted time string
static Value native_time_format(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_INT(args[0]) || !IS_STRING(args[1])) {
        return VAL_NIL;
    }
    time_t epoch = (time_t)AS_INT(args[0]);
    ObjString* fmt = AS_STRING(args[1]);

    struct tm tm_buf;
    localtime_r(&epoch, &tm_buf);

    char buffer[256];
    size_t len = strftime(buffer, sizeof(buffer), fmt->data, &tm_buf);
    if (len == 0) return VAL_NIL;

    return OBJ_VAL(obj_string_new(buffer, (uint32_t)len));
}

void stdlib_time_init(ObjMap* pkg) {
    stdlib_register(pkg, "now", native_time_now, 0);
    stdlib_register(pkg, "ms", native_time_ms, 0);
    stdlib_register(pkg, "format", native_time_format, 2);
}
