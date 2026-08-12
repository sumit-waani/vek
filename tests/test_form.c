/*
 * Unit tests for the form validation stdlib package.
 * Tests URL decoding, form parsing, and field validation.
 */
#include "common.h"
#include "value.h"
#include "memory.h"
#include "gc.h"
#include "object.h"
#include "vm.h"
#include "vek_stdlib.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  test: %s ... ", #name); \
    if (test_##name()) { tests_passed++; printf("ok\n"); } \
    else { printf("FAILED\n"); } \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("\n    ASSERT FAILED: %s (line %d)\n", #cond, __LINE__); \
        return false; \
    } \
} while(0)

// Helper: call a native function registered in a package
static Value call_form_fn(const char* name, int argc, Value* args) {
    ObjString* pkg_key = obj_string_new("form", 4);
    Value pkg_val;
    if (!obj_map_get(vm.globals, pkg_key, &pkg_val)) return VAL_NIL;
    ObjMap* pkg = AS_MAP(pkg_val);

    ObjString* fn_key = obj_string_new(name, (uint32_t)strlen(name));
    Value fn_val;
    if (!obj_map_get(pkg, fn_key, &fn_val)) return VAL_NIL;
    ObjNative* native = AS_NATIVE(fn_val);

    return native->function(argc, args);
}

// ---- Form Parse Tests ----

static bool test_parse_simple(void) {
    Value args[1];
    args[0] = OBJ_VAL(obj_string_new("name=alice&age=30", 17));
    Value result = call_form_fn("parse", 1, args);
    ASSERT(IS_MAP(result));

    ObjMap* map = AS_MAP(result);
    Value val;

    ObjString* name_key = obj_string_new("name", 4);
    ASSERT(obj_map_get(map, name_key, &val));
    ASSERT(IS_STRING(val));
    ASSERT(strcmp(AS_STRING(val)->data, "alice") == 0);

    ObjString* age_key = obj_string_new("age", 3);
    ASSERT(obj_map_get(map, age_key, &val));
    ASSERT(IS_STRING(val));
    ASSERT(strcmp(AS_STRING(val)->data, "30") == 0);

    return true;
}

static bool test_parse_url_decode(void) {
    // %20 is space, + is space, %40 is @
    Value args[1];
    args[0] = OBJ_VAL(obj_string_new("email=user%40example.com&msg=hello+world", 40));
    Value result = call_form_fn("parse", 1, args);
    ASSERT(IS_MAP(result));

    ObjMap* map = AS_MAP(result);
    Value val;

    ObjString* email_key = obj_string_new("email", 5);
    ASSERT(obj_map_get(map, email_key, &val));
    ASSERT(IS_STRING(val));
    ASSERT(strcmp(AS_STRING(val)->data, "user@example.com") == 0);

    ObjString* msg_key = obj_string_new("msg", 3);
    ASSERT(obj_map_get(map, msg_key, &val));
    ASSERT(IS_STRING(val));
    ASSERT(strcmp(AS_STRING(val)->data, "hello world") == 0);

    return true;
}

static bool test_parse_empty_value(void) {
    Value args[1];
    args[0] = OBJ_VAL(obj_string_new("key=&other=val", 14));
    Value result = call_form_fn("parse", 1, args);
    ASSERT(IS_MAP(result));

    ObjMap* map = AS_MAP(result);
    Value val;

    ObjString* key = obj_string_new("key", 3);
    ASSERT(obj_map_get(map, key, &val));
    ASSERT(IS_STRING(val));
    ASSERT(AS_STRING(val)->length == 0);

    return true;
}

static bool test_parse_no_equals(void) {
    // Key without = should have empty string value
    Value args[1];
    args[0] = OBJ_VAL(obj_string_new("solo", 4));
    Value result = call_form_fn("parse", 1, args);
    ASSERT(IS_MAP(result));

    ObjMap* map = AS_MAP(result);
    Value val;

    ObjString* key = obj_string_new("solo", 4);
    ASSERT(obj_map_get(map, key, &val));
    ASSERT(IS_STRING(val));
    ASSERT(AS_STRING(val)->length == 0);

    return true;
}

// ---- Validation Tests ----

static bool test_validate_required_pass(void) {
    // Create parsed data
    Value parse_args[1];
    parse_args[0] = OBJ_VAL(obj_string_new("name=alice", 10));
    Value parsed = call_form_fn("parse", 1, parse_args);
    ASSERT(IS_MAP(parsed));

    // Create rules: {name: {required: true}}
    ObjMap* rules = obj_map_new();
    gc_push_root(OBJ_VAL(rules));
    ObjMap* name_rule = obj_map_new();
    gc_push_root(OBJ_VAL(name_rule));
    ObjString* req_key = obj_string_new("required", 8);
    obj_map_set(name_rule, req_key, BOOL_VAL(true));
    ObjString* name_key = obj_string_new("name", 4);
    obj_map_set(rules, name_key, OBJ_VAL(name_rule));
    gc_pop_root();
    gc_pop_root();

    Value validate_args[2];
    validate_args[0] = parsed;
    validate_args[1] = OBJ_VAL(rules);
    Value result = call_form_fn("validate", 2, validate_args);
    ASSERT(result == VAL_TRUE);

    // form.valid() should be true
    Value valid_result = call_form_fn("valid", 0, NULL);
    ASSERT(valid_result == VAL_TRUE);

    return true;
}

static bool test_validate_required_fail(void) {
    // Create parsed data with empty field
    Value parse_args[1];
    parse_args[0] = OBJ_VAL(obj_string_new("name=", 5));
    Value parsed = call_form_fn("parse", 1, parse_args);
    ASSERT(IS_MAP(parsed));

    // Create rules: {name: {required: true}}
    ObjMap* rules = obj_map_new();
    gc_push_root(OBJ_VAL(rules));
    ObjMap* name_rule = obj_map_new();
    gc_push_root(OBJ_VAL(name_rule));
    ObjString* req_key = obj_string_new("required", 8);
    obj_map_set(name_rule, req_key, BOOL_VAL(true));
    ObjString* name_key = obj_string_new("name", 4);
    obj_map_set(rules, name_key, OBJ_VAL(name_rule));
    gc_pop_root();
    gc_pop_root();

    Value validate_args[2];
    validate_args[0] = parsed;
    validate_args[1] = OBJ_VAL(rules);
    Value result = call_form_fn("validate", 2, validate_args);
    ASSERT(result == VAL_FALSE);

    // form.valid() should be false
    Value valid_result = call_form_fn("valid", 0, NULL);
    ASSERT(valid_result == VAL_FALSE);

    // form.errors() should have one error
    Value errors = call_form_fn("errors", 0, NULL);
    ASSERT(IS_LIST(errors));
    ObjList* err_list = AS_LIST(errors);
    ASSERT(err_list->length == 1);

    // Check the error map
    Value err = obj_list_get(err_list, 0);
    ASSERT(IS_MAP(err));
    ObjMap* err_map = AS_MAP(err);
    Value field_val;
    ObjString* field_key = obj_string_new("field", 5);
    ASSERT(obj_map_get(err_map, field_key, &field_val));
    ASSERT(IS_STRING(field_val));
    ASSERT(strcmp(AS_STRING(field_val)->data, "name") == 0);

    Value msg_val;
    ObjString* msg_key = obj_string_new("message", 7);
    ASSERT(obj_map_get(err_map, msg_key, &msg_val));
    ASSERT(IS_STRING(msg_val));
    ASSERT(strcmp(AS_STRING(msg_val)->data, "is required") == 0);

    return true;
}

static bool test_validate_min_length(void) {
    Value parse_args[1];
    parse_args[0] = OBJ_VAL(obj_string_new("pw=abc", 6));
    Value parsed = call_form_fn("parse", 1, parse_args);

    // Rules: {pw: {min: 5}}
    ObjMap* rules = obj_map_new();
    gc_push_root(OBJ_VAL(rules));
    ObjMap* pw_rule = obj_map_new();
    gc_push_root(OBJ_VAL(pw_rule));
    ObjString* min_key = obj_string_new("min", 3);
    obj_map_set(pw_rule, min_key, INT_VAL(5));
    ObjString* pw_key = obj_string_new("pw", 2);
    obj_map_set(rules, pw_key, OBJ_VAL(pw_rule));
    gc_pop_root();
    gc_pop_root();

    Value validate_args[2];
    validate_args[0] = parsed;
    validate_args[1] = OBJ_VAL(rules);
    Value result = call_form_fn("validate", 2, validate_args);
    ASSERT(result == VAL_FALSE);

    Value errors = call_form_fn("errors", 0, NULL);
    ASSERT(IS_LIST(errors));
    ASSERT(AS_LIST(errors)->length == 1);

    return true;
}

static bool test_validate_max_length(void) {
    Value parse_args[1];
    parse_args[0] = OBJ_VAL(obj_string_new("name=toolongname", 16));
    Value parsed = call_form_fn("parse", 1, parse_args);

    // Rules: {name: {max: 5}}
    ObjMap* rules = obj_map_new();
    gc_push_root(OBJ_VAL(rules));
    ObjMap* name_rule = obj_map_new();
    gc_push_root(OBJ_VAL(name_rule));
    ObjString* max_key = obj_string_new("max", 3);
    obj_map_set(name_rule, max_key, INT_VAL(5));
    ObjString* name_key = obj_string_new("name", 4);
    obj_map_set(rules, name_key, OBJ_VAL(name_rule));
    gc_pop_root();
    gc_pop_root();

    Value validate_args[2];
    validate_args[0] = parsed;
    validate_args[1] = OBJ_VAL(rules);
    Value result = call_form_fn("validate", 2, validate_args);
    ASSERT(result == VAL_FALSE);

    return true;
}

static bool test_validate_type_email_valid(void) {
    Value parse_args[1];
    parse_args[0] = OBJ_VAL(obj_string_new("email=user%40example.com", 24));
    Value parsed = call_form_fn("parse", 1, parse_args);

    // Rules: {email: {type: "email"}}
    ObjMap* rules = obj_map_new();
    gc_push_root(OBJ_VAL(rules));
    ObjMap* email_rule = obj_map_new();
    gc_push_root(OBJ_VAL(email_rule));
    ObjString* type_key = obj_string_new("type", 4);
    ObjString* email_type = obj_string_new("email", 5);
    obj_map_set(email_rule, type_key, OBJ_VAL(email_type));
    ObjString* email_key = obj_string_new("email", 5);
    obj_map_set(rules, email_key, OBJ_VAL(email_rule));
    gc_pop_root();
    gc_pop_root();

    Value validate_args[2];
    validate_args[0] = parsed;
    validate_args[1] = OBJ_VAL(rules);
    Value result = call_form_fn("validate", 2, validate_args);
    ASSERT(result == VAL_TRUE);

    return true;
}

static bool test_validate_type_email_invalid(void) {
    Value parse_args[1];
    parse_args[0] = OBJ_VAL(obj_string_new("email=notanemail", 16));
    Value parsed = call_form_fn("parse", 1, parse_args);

    ObjMap* rules = obj_map_new();
    gc_push_root(OBJ_VAL(rules));
    ObjMap* email_rule = obj_map_new();
    gc_push_root(OBJ_VAL(email_rule));
    ObjString* type_key = obj_string_new("type", 4);
    ObjString* email_type = obj_string_new("email", 5);
    obj_map_set(email_rule, type_key, OBJ_VAL(email_type));
    ObjString* email_key = obj_string_new("email", 5);
    obj_map_set(rules, email_key, OBJ_VAL(email_rule));
    gc_pop_root();
    gc_pop_root();

    Value validate_args[2];
    validate_args[0] = parsed;
    validate_args[1] = OBJ_VAL(rules);
    Value result = call_form_fn("validate", 2, validate_args);
    ASSERT(result == VAL_FALSE);

    return true;
}

static bool test_validate_type_int_valid(void) {
    Value parse_args[1];
    parse_args[0] = OBJ_VAL(obj_string_new("count=42", 8));
    Value parsed = call_form_fn("parse", 1, parse_args);

    ObjMap* rules = obj_map_new();
    gc_push_root(OBJ_VAL(rules));
    ObjMap* count_rule = obj_map_new();
    gc_push_root(OBJ_VAL(count_rule));
    ObjString* type_key = obj_string_new("type", 4);
    ObjString* int_type = obj_string_new("int", 3);
    obj_map_set(count_rule, type_key, OBJ_VAL(int_type));
    ObjString* count_key = obj_string_new("count", 5);
    obj_map_set(rules, count_key, OBJ_VAL(count_rule));
    gc_pop_root();
    gc_pop_root();

    Value validate_args[2];
    validate_args[0] = parsed;
    validate_args[1] = OBJ_VAL(rules);
    Value result = call_form_fn("validate", 2, validate_args);
    ASSERT(result == VAL_TRUE);

    return true;
}

static bool test_validate_type_int_invalid(void) {
    Value parse_args[1];
    parse_args[0] = OBJ_VAL(obj_string_new("count=abc", 9));
    Value parsed = call_form_fn("parse", 1, parse_args);

    ObjMap* rules = obj_map_new();
    gc_push_root(OBJ_VAL(rules));
    ObjMap* count_rule = obj_map_new();
    gc_push_root(OBJ_VAL(count_rule));
    ObjString* type_key = obj_string_new("type", 4);
    ObjString* int_type = obj_string_new("int", 3);
    obj_map_set(count_rule, type_key, OBJ_VAL(int_type));
    ObjString* count_key = obj_string_new("count", 5);
    obj_map_set(rules, count_key, OBJ_VAL(count_rule));
    gc_pop_root();
    gc_pop_root();

    Value validate_args[2];
    validate_args[0] = parsed;
    validate_args[1] = OBJ_VAL(rules);
    Value result = call_form_fn("validate", 2, validate_args);
    ASSERT(result == VAL_FALSE);

    return true;
}

static bool test_form_get(void) {
    Value parse_args[1];
    parse_args[0] = OBJ_VAL(obj_string_new("color=blue&size=large", 21));
    call_form_fn("parse", 1, parse_args);

    Value get_args[1];
    get_args[0] = OBJ_VAL(obj_string_new("color", 5));
    Value result = call_form_fn("get", 1, get_args);
    ASSERT(IS_STRING(result));
    ASSERT(strcmp(AS_STRING(result)->data, "blue") == 0);

    get_args[0] = OBJ_VAL(obj_string_new("size", 4));
    result = call_form_fn("get", 1, get_args);
    ASSERT(IS_STRING(result));
    ASSERT(strcmp(AS_STRING(result)->data, "large") == 0);

    get_args[0] = OBJ_VAL(obj_string_new("missing", 7));
    result = call_form_fn("get", 1, get_args);
    ASSERT(IS_NIL(result));

    return true;
}

int main(void) {
    gc_init();
    intern_table_init();
    heap_init();
    vm_init();
    stdlib_init();

    printf("=== Form Validation Tests ===\n");

    // Parse tests
    TEST(parse_simple);
    TEST(parse_url_decode);
    TEST(parse_empty_value);
    TEST(parse_no_equals);

    // Validation tests
    TEST(validate_required_pass);
    TEST(validate_required_fail);
    TEST(validate_min_length);
    TEST(validate_max_length);
    TEST(validate_type_email_valid);
    TEST(validate_type_email_invalid);
    TEST(validate_type_int_valid);
    TEST(validate_type_int_invalid);

    // Get test
    TEST(form_get);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);

    vm_free();
    intern_table_destroy();
    gc_destroy();
    heap_destroy();

    return tests_passed == tests_run ? 0 : 1;
}
