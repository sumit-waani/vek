#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

// ---- assert(condition, message?) ----
// Fails if condition is falsy

static Value native_assert(int arg_count, Value* args) {
    if (IS_FALSY(args[0])) {
        vm.had_error = true;
        if (arg_count >= 2 && IS_STRING(args[1])) {
            ObjString* msg = AS_STRING(args[1]);
            snprintf(vm.error_msg, sizeof(vm.error_msg), "Assertion failed: %.*s",
                     (int)msg->length, msg->data);
        } else {
            snprintf(vm.error_msg, sizeof(vm.error_msg), "Assertion failed: expected truthy value");
        }
    }
    return VAL_NIL;
}

// ---- assert_eq(expected, actual, message?) ----
// Fails if expected != actual

static void format_value(char* buf, size_t buf_size, Value value) {
    if (IS_NIL(value)) {
        snprintf(buf, buf_size, "nil");
    } else if (value == VAL_TRUE) {
        snprintf(buf, buf_size, "true");
    } else if (value == VAL_FALSE) {
        snprintf(buf, buf_size, "false");
    } else if (IS_INT(value)) {
        snprintf(buf, buf_size, "%lld", (long long)AS_INT(value));
    } else if (IS_FLOAT(value)) {
        snprintf(buf, buf_size, "%g", AS_DOUBLE(value));
    } else if (IS_STRING(value)) {
        ObjString* s = AS_STRING(value);
        snprintf(buf, buf_size, "\"%.*s\"", (int)(s->length > 64 ? 64 : s->length), s->data);
    } else if (IS_PTR(value)) {
        snprintf(buf, buf_size, "<object>");
    } else {
        snprintf(buf, buf_size, "<unknown>");
    }
}

static Value native_assert_eq(int arg_count, Value* args) {
    Value expected = args[0];
    Value actual = args[1];

    if (!value_equal(expected, actual)) {
        vm.had_error = true;
        char exp_buf[128];
        char act_buf[128];
        format_value(exp_buf, sizeof(exp_buf), expected);
        format_value(act_buf, sizeof(act_buf), actual);

        if (arg_count >= 3 && IS_STRING(args[2])) {
            ObjString* msg = AS_STRING(args[2]);
            snprintf(vm.error_msg, sizeof(vm.error_msg),
                     "Assertion failed: %.*s\n  expected: %s\n    actual: %s",
                     (int)msg->length, msg->data, exp_buf, act_buf);
        } else {
            snprintf(vm.error_msg, sizeof(vm.error_msg),
                     "Assertion failed: values not equal\n  expected: %s\n    actual: %s",
                     exp_buf, act_buf);
        }
    }
    return VAL_NIL;
}

// ---- assert_ne(a, b, message?) ----
// Fails if a == b

static Value native_assert_ne(int arg_count, Value* args) {
    Value a = args[0];
    Value b = args[1];

    if (value_equal(a, b)) {
        vm.had_error = true;
        char val_buf[128];
        format_value(val_buf, sizeof(val_buf), a);

        if (arg_count >= 3 && IS_STRING(args[2])) {
            ObjString* msg = AS_STRING(args[2]);
            snprintf(vm.error_msg, sizeof(vm.error_msg),
                     "Assertion failed: %.*s\n  both values equal: %s",
                     (int)msg->length, msg->data, val_buf);
        } else {
            snprintf(vm.error_msg, sizeof(vm.error_msg),
                     "Assertion failed: expected values to differ, but both are: %s",
                     val_buf);
        }
    }
    return VAL_NIL;
}

// ---- Package init ----

void stdlib_test_init(ObjMap* pkg) {
    stdlib_register(pkg, "assert", native_assert, -1);
    stdlib_register(pkg, "assert_eq", native_assert_eq, -1);
    stdlib_register(pkg, "assert_ne", native_assert_ne, -1);
}
