/*
 * Unit tests for the db (SQLite) stdlib package.
 * Tests function registration and database operations
 * using an in-memory SQLite database.
 */
#define _POSIX_C_SOURCE 200809L
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
static Value call_db_fn(const char* name, int argc, Value* args) {
    // Look up the db package
    ObjString* db_key = obj_string_new("db", 2);
    Value db_val;
    if (!obj_map_get(vm.globals, db_key, &db_val)) return VAL_NIL;
    ObjMap* db_pkg = AS_MAP(db_val);

    // Look up the function
    ObjString* fn_key = obj_string_new(name, (uint32_t)strlen(name));
    Value fn_val;
    if (!obj_map_get(db_pkg, fn_key, &fn_val)) return VAL_NIL;
    ObjNative* native = AS_NATIVE(fn_val);

    return native->function(argc, args);
}

// ---- Tests ----

// Test that db package is registered with expected functions
static bool test_package_registered(void) {
    ObjString* db_key = obj_string_new("db", 2);
    Value db_val;
    ASSERT(obj_map_get(vm.globals, db_key, &db_val));
    ASSERT(IS_MAP(db_val));

    ObjMap* db_pkg = AS_MAP(db_val);

    // Check that expected functions are registered
    ObjString* connect_key = obj_string_new("connect", 7);
    Value connect_val;
    ASSERT(obj_map_get(db_pkg, connect_key, &connect_val));
    ASSERT(IS_NATIVE(connect_val));

    ObjString* query_key = obj_string_new("query", 5);
    Value query_val;
    ASSERT(obj_map_get(db_pkg, query_key, &query_val));
    ASSERT(IS_NATIVE(query_val));

    ObjString* exec_key = obj_string_new("exec", 4);
    Value exec_val;
    ASSERT(obj_map_get(db_pkg, exec_key, &exec_val));
    ASSERT(IS_NATIVE(exec_val));

    ObjString* close_key = obj_string_new("close", 5);
    Value close_val;
    ASSERT(obj_map_get(db_pkg, close_key, &close_val));
    ASSERT(IS_NATIVE(close_val));

    ObjString* tx_key = obj_string_new("transaction", 11);
    Value tx_val;
    ASSERT(obj_map_get(db_pkg, tx_key, &tx_val));
    ASSERT(IS_NATIVE(tx_val));

    return true;
}

// Test that db.connect succeeds with :memory: database
static bool test_connect_memory(void) {
    Value result = call_db_fn("connect", 0, NULL);
    // With DATABASE_PATH=:memory:, connect should succeed
    ASSERT(!IS_NIL(result));
    ASSERT(IS_BOOL(result));
    ASSERT(AS_BOOL(result) == true);
    return true;
}

// Test that db.exec works with actual SQL
static bool test_exec_create_table(void) {
    Value args[1];
    args[0] = OBJ_VAL(obj_string_new(
        "CREATE TABLE IF NOT EXISTS test_users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)", 90));
    Value result = call_db_fn("exec", 1, args);
    // exec returns last_insert_rowid (0 for DDL)
    ASSERT(!IS_NIL(result));
    return true;
}

// Test that db.exec can insert data
static bool test_exec_insert(void) {
    Value args[3];
    args[0] = OBJ_VAL(obj_string_new("INSERT INTO test_users (name, age) VALUES (?, ?)", 49));
    args[1] = OBJ_VAL(obj_string_new("Alice", 5));
    args[2] = INT_VAL(30);
    Value result = call_db_fn("exec", 3, args);
    ASSERT(!IS_NIL(result));
    // Should return the last_insert_rowid (1 for first insert)
    ASSERT(IS_INT(result));
    ASSERT(AS_INT(result) == 1);
    return true;
}

// Test that db.query returns results
static bool test_query_select(void) {
    Value args[1];
    args[0] = OBJ_VAL(obj_string_new("SELECT id, name, age FROM test_users", 36));
    Value result = call_db_fn("query", 1, args);
    ASSERT(!IS_NIL(result));
    ASSERT(IS_LIST(result));

    ObjList* list = AS_LIST(result);
    ASSERT(list->length == 1);

    // First row should be a map with id=1, name="Alice", age=30
    Value row = list->data[0];
    ASSERT(IS_MAP(row));

    ObjMap* map = AS_MAP(row);
    ObjString* name_key = obj_string_new("name", 4);
    Value name_val;
    ASSERT(obj_map_get(map, name_key, &name_val));
    ASSERT(IS_STRING(name_val));
    ObjString* name_str = AS_STRING(name_val);
    ASSERT(name_str->length == 5);
    ASSERT(memcmp(name_str->data, "Alice", 5) == 0);

    ObjString* age_key = obj_string_new("age", 3);
    Value age_val;
    ASSERT(obj_map_get(map, age_key, &age_val));
    ASSERT(IS_INT(age_val));
    ASSERT(AS_INT(age_val) == 30);

    return true;
}

// Test that query with invalid args returns nil
static bool test_query_invalid_args(void) {
    Value args[1];
    args[0] = INT_VAL(42); // not a string
    Value result = call_db_fn("query", 1, args);
    ASSERT(IS_NIL(result));
    return true;
}

// Test that exec with invalid args returns nil
static bool test_exec_invalid_args(void) {
    Value args[1];
    args[0] = VAL_NIL; // not a string
    Value result = call_db_fn("exec", 1, args);
    ASSERT(IS_NIL(result));
    return true;
}

// Test that db.close does not crash
static bool test_close(void) {
    Value result = call_db_fn("close", 0, NULL);
    ASSERT(IS_NIL(result));
    return true;
}

int main(void) {
    // Use in-memory SQLite database for testing
    setenv("DATABASE_PATH", ":memory:", 1);

    // Initialize subsystems (same as main.c)
    gc_init();
    intern_table_init();
    heap_init();
    vm_init();
    stdlib_init();

    printf("=== DB (SQLite) Tests ===\n");

    TEST(package_registered);
    TEST(connect_memory);
    TEST(exec_create_table);
    TEST(exec_insert);
    TEST(query_select);
    TEST(query_invalid_args);
    TEST(exec_invalid_args);
    TEST(close);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);

    // Cleanup
    vm_free();
    intern_table_destroy();
    gc_destroy();
    heap_destroy();

    return tests_passed == tests_run ? 0 : 1;
}
