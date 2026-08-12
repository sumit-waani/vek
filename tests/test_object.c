/*
 * Unit tests for the object system.
 * Tests allocation of ObjString, ObjList, ObjMap.
 */
#include "common.h"
#include "value.h"
#include "memory.h"
#include "gc.h"
#include "object.h"

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

// ---- String tests ----

static bool test_string_create(void) {
    ObjString* s = obj_string_new("hello", 5);
    ASSERT(s != NULL);
    ASSERT(s->length == 5);
    ASSERT(strcmp(s->data, "hello") == 0);
    ASSERT(s->header.type == OBJ_STRING);
    return true;
}

static bool test_string_interning(void) {
    ObjString* a = obj_string_new("test", 4);
    ObjString* b = obj_string_new("test", 4);
    // Interned strings should be the same pointer
    ASSERT(a == b);
    return true;
}

static bool test_string_different(void) {
    ObjString* a = obj_string_new("foo", 3);
    ObjString* b = obj_string_new("bar", 3);
    ASSERT(a != b);
    ASSERT(strcmp(a->data, "foo") == 0);
    ASSERT(strcmp(b->data, "bar") == 0);
    return true;
}

static bool test_string_empty(void) {
    ObjString* s = obj_string_new("", 0);
    ASSERT(s != NULL);
    ASSERT(s->length == 0);
    ASSERT(s->data[0] == '\0');
    return true;
}

static bool test_string_hash(void) {
    ObjString* a = obj_string_new("hello", 5);
    ObjString* b = obj_string_new("world", 5);
    // Different strings should have different hashes (not guaranteed but very likely)
    ASSERT(a->hash != b->hash);
    return true;
}

// ---- List tests ----

static bool test_list_create(void) {
    ObjList* list = obj_list_new();
    ASSERT(list != NULL);
    ASSERT(list->length == 0);
    ASSERT(list->header.type == OBJ_LIST);
    return true;
}

static bool test_list_push(void) {
    ObjList* list = obj_list_new();
    obj_list_push(list, INT_VAL(42));
    ASSERT(list->length == 1);
    ASSERT(AS_INT(obj_list_get(list, 0)) == 42);
    return true;
}

static bool test_list_push_multiple(void) {
    ObjList* list = obj_list_new();
    for (int i = 0; i < 100; i++) {
        obj_list_push(list, INT_VAL(i));
    }
    ASSERT(list->length == 100);
    ASSERT(AS_INT(obj_list_get(list, 0)) == 0);
    ASSERT(AS_INT(obj_list_get(list, 99)) == 99);
    ASSERT(AS_INT(obj_list_get(list, 50)) == 50);
    return true;
}

static bool test_list_mixed_values(void) {
    ObjList* list = obj_list_new();
    obj_list_push(list, INT_VAL(1));
    obj_list_push(list, FLOAT_VAL(3.14));
    obj_list_push(list, BOOL_VAL(true));
    obj_list_push(list, NIL_VAL);

    ASSERT(list->length == 4);
    ASSERT(IS_INT(obj_list_get(list, 0)));
    ASSERT(IS_FLOAT(obj_list_get(list, 1)));
    ASSERT(IS_BOOL(obj_list_get(list, 2)));
    ASSERT(IS_NIL(obj_list_get(list, 3)));
    return true;
}

// ---- Map tests ----

static bool test_map_create(void) {
    ObjMap* map = obj_map_new();
    ASSERT(map != NULL);
    ASSERT(map->length == 0);
    ASSERT(map->header.type == OBJ_MAP);
    return true;
}

static bool test_map_set_get(void) {
    ObjMap* map = obj_map_new();
    ObjString* key = obj_string_new("name", 4);
    obj_map_set(map, key, INT_VAL(42));

    Value result;
    ASSERT(obj_map_get(map, key, &result));
    ASSERT(AS_INT(result) == 42);
    return true;
}

static bool test_map_overwrite(void) {
    ObjMap* map = obj_map_new();
    ObjString* key = obj_string_new("x", 1);
    obj_map_set(map, key, INT_VAL(1));
    obj_map_set(map, key, INT_VAL(2));

    ASSERT(map->length == 1);
    Value result;
    ASSERT(obj_map_get(map, key, &result));
    ASSERT(AS_INT(result) == 2);
    return true;
}

static bool test_map_multiple_keys(void) {
    ObjMap* map = obj_map_new();
    ObjString* k1 = obj_string_new("a", 1);
    ObjString* k2 = obj_string_new("b", 1);
    ObjString* k3 = obj_string_new("c", 1);

    obj_map_set(map, k1, INT_VAL(1));
    obj_map_set(map, k2, INT_VAL(2));
    obj_map_set(map, k3, INT_VAL(3));

    ASSERT(map->length == 3);

    Value v;
    ASSERT(obj_map_get(map, k1, &v) && AS_INT(v) == 1);
    ASSERT(obj_map_get(map, k2, &v) && AS_INT(v) == 2);
    ASSERT(obj_map_get(map, k3, &v) && AS_INT(v) == 3);
    return true;
}

static bool test_map_get_missing(void) {
    ObjMap* map = obj_map_new();
    ObjString* key = obj_string_new("missing", 7);
    Value v;
    ASSERT(!obj_map_get(map, key, &v));
    return true;
}

static bool test_map_delete(void) {
    ObjMap* map = obj_map_new();
    ObjString* key = obj_string_new("del", 3);
    obj_map_set(map, key, INT_VAL(99));
    ASSERT(map->length == 1);

    ASSERT(obj_map_delete(map, key));
    ASSERT(map->length == 0);

    Value v;
    ASSERT(!obj_map_get(map, key, &v));
    return true;
}

// ---- Bytes tests ----

static bool test_bytes_create(void) {
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    ObjBytes* bytes = obj_bytes_new(data, 4);
    ASSERT(bytes != NULL);
    ASSERT(bytes->length == 4);
    ASSERT(bytes->data[0] == 0xDE);
    ASSERT(bytes->data[3] == 0xEF);
    ASSERT(bytes->header.type == OBJ_BYTES);
    return true;
}

// ---- Closure tests ----

static bool test_function_create(void) {
    ObjFunction* func = obj_function_new();
    ASSERT(func != NULL);
    ASSERT(func->arity == 0);
    ASSERT(func->header.type == OBJ_FUNCTION);
    return true;
}

static bool test_closure_create(void) {
    ObjFunction* func = obj_function_new();
    ObjClosure* closure = obj_closure_new(func);
    ASSERT(closure != NULL);
    ASSERT(closure->function == func);
    ASSERT(closure->upvalue_count == 0);
    ASSERT(closure->header.type == OBJ_CLOSURE);
    return true;
}

int main(void) {
    // Initialize subsystems
    gc_init();
    intern_table_init();
    heap_init();

    // Disable GC during tests to avoid premature collection
    gc.enabled = false;

    printf("=== Object System Tests ===\n");

    TEST(string_create);
    TEST(string_interning);
    TEST(string_different);
    TEST(string_empty);
    TEST(string_hash);

    TEST(list_create);
    TEST(list_push);
    TEST(list_push_multiple);
    TEST(list_mixed_values);

    TEST(map_create);
    TEST(map_set_get);
    TEST(map_overwrite);
    TEST(map_multiple_keys);
    TEST(map_get_missing);
    TEST(map_delete);

    TEST(bytes_create);

    TEST(function_create);
    TEST(closure_create);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);

    // Cleanup
    intern_table_destroy();
    gc_destroy();
    heap_destroy();

    return tests_passed == tests_run ? 0 : 1;
}
