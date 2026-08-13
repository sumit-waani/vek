/*
 * Unit tests for the db (Turso) stdlib package.
 * Tests function registration and graceful handling when
 * Turso env vars are not set (should not crash).
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

// Test that db.connect returns nil when env vars are not set
static bool test_connect_no_env(void) {
    // Ensure TURSO env vars are not set (test environment)
    Value result = call_db_fn("connect", 0, NULL);
    // Without env vars set, connect should return nil
    ASSERT(IS_NIL(result));
    return true;
}

// Test that db.query returns nil when not connected
static bool test_query_no_connection(void) {
    Value args[1];
    args[0] = OBJ_VAL(obj_string_new("SELECT 1", 8));
    Value result = call_db_fn("query", 1, args);
    ASSERT(IS_NIL(result));
    return true;
}

// Test that db.exec returns nil when not connected
static bool test_exec_no_connection(void) {
    Value args[1];
    args[0] = OBJ_VAL(obj_string_new("INSERT INTO test VALUES (1)", 27));
    Value result = call_db_fn("exec", 1, args);
    ASSERT(IS_NIL(result));
    return true;
}

// Test that db.close does not crash when not connected
static bool test_close_no_connection(void) {
    Value result = call_db_fn("close", 0, NULL);
    ASSERT(IS_NIL(result));
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

int main(void) {
    // Unset Turso env vars to test graceful handling
    unsetenv("TURSO_DATABASE_URL");
    unsetenv("TURSO_AUTH_TOKEN");

    // Initialize subsystems (same as main.c)
    gc_init();
    intern_table_init();
    heap_init();
    vm_init();
    stdlib_init();

    printf("=== DB (Turso) Tests ===\n");

    TEST(package_registered);
    TEST(connect_no_env);
    TEST(query_no_connection);
    TEST(exec_no_connection);
    TEST(close_no_connection);
    TEST(query_invalid_args);
    TEST(exec_invalid_args);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);

    // Cleanup
    vm_free();
    intern_table_destroy();
    gc_destroy();
    heap_destroy();

    return tests_passed == tests_run ? 0 : 1;
}
