/*
 * Unit tests for the db (SQLite) stdlib package.
 * Tests in-memory database operations.
 */
#include "common.h"
#include "value.h"
#include "memory.h"
#include "gc.h"
#include "object.h"
#include "vm.h"
#include "vek_stdlib.h"
#include "sqlite3.h"

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

static bool test_open_memory(void) {
    Value args[1];
    args[0] = OBJ_VAL(obj_string_new(":memory:", 8));
    Value result = call_db_fn("open", 1, args);
    ASSERT(result == VAL_TRUE);
    return true;
}

static bool test_exec_create_table(void) {
    Value args[1];
    args[0] = OBJ_VAL(obj_string_new(
        "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)", 70));
    Value result = call_db_fn("exec", 1, args);
    // Should return 0 (last_insert_rowid for DDL)
    ASSERT(IS_INT(result));
    return true;
}

static bool test_exec_insert(void) {
    Value args[3];
    args[0] = OBJ_VAL(obj_string_new("INSERT INTO users (name, age) VALUES (?, ?)", 44));
    args[1] = OBJ_VAL(obj_string_new("Alice", 5));
    args[2] = INT_VAL(30);
    Value result = call_db_fn("exec", 3, args);
    ASSERT(IS_INT(result));
    ASSERT(AS_INT(result) == 1); // first insert rowid = 1
    return true;
}

static bool test_exec_insert_second(void) {
    Value args[3];
    args[0] = OBJ_VAL(obj_string_new("INSERT INTO users (name, age) VALUES (?, ?)", 44));
    args[1] = OBJ_VAL(obj_string_new("Bob", 3));
    args[2] = INT_VAL(25);
    Value result = call_db_fn("exec", 3, args);
    ASSERT(IS_INT(result));
    ASSERT(AS_INT(result) == 2); // second insert rowid = 2
    return true;
}

static bool test_query_all(void) {
    Value args[1];
    args[0] = OBJ_VAL(obj_string_new("SELECT id, name, age FROM users ORDER BY id", 44));
    Value result = call_db_fn("query", 1, args);
    ASSERT(IS_LIST(result));
    ObjList* list = AS_LIST(result);
    ASSERT(list->length == 2);

    // Check first row
    Value row0 = obj_list_get(list, 0);
    ASSERT(IS_MAP(row0));
    ObjMap* map0 = AS_MAP(row0);

    Value id_val;
    ObjString* id_key = obj_string_new("id", 2);
    ASSERT(obj_map_get(map0, id_key, &id_val));
    ASSERT(IS_INT(id_val));
    ASSERT(AS_INT(id_val) == 1);

    Value name_val;
    ObjString* name_key = obj_string_new("name", 4);
    ASSERT(obj_map_get(map0, name_key, &name_val));
    ASSERT(IS_STRING(name_val));
    ASSERT(strcmp(AS_STRING(name_val)->data, "Alice") == 0);

    Value age_val;
    ObjString* age_key = obj_string_new("age", 3);
    ASSERT(obj_map_get(map0, age_key, &age_val));
    ASSERT(IS_INT(age_val));
    ASSERT(AS_INT(age_val) == 30);

    return true;
}

static bool test_query_with_params(void) {
    Value args[2];
    args[0] = OBJ_VAL(obj_string_new("SELECT name FROM users WHERE age > ?", 36));
    args[1] = INT_VAL(26);
    Value result = call_db_fn("query", 2, args);
    ASSERT(IS_LIST(result));
    ObjList* list = AS_LIST(result);
    ASSERT(list->length == 1);

    Value row0 = obj_list_get(list, 0);
    ObjMap* map0 = AS_MAP(row0);
    Value name_val;
    ObjString* name_key = obj_string_new("name", 4);
    ASSERT(obj_map_get(map0, name_key, &name_val));
    ASSERT(strcmp(AS_STRING(name_val)->data, "Alice") == 0);
    return true;
}

static bool test_row_found(void) {
    Value args[2];
    args[0] = OBJ_VAL(obj_string_new("SELECT name, age FROM users WHERE id = ?", 40));
    args[1] = INT_VAL(1);
    Value result = call_db_fn("row", 2, args);
    ASSERT(IS_MAP(result));
    ObjMap* map = AS_MAP(result);

    Value name_val;
    ObjString* name_key = obj_string_new("name", 4);
    ASSERT(obj_map_get(map, name_key, &name_val));
    ASSERT(strcmp(AS_STRING(name_val)->data, "Alice") == 0);
    return true;
}

static bool test_row_not_found(void) {
    Value args[2];
    args[0] = OBJ_VAL(obj_string_new("SELECT name FROM users WHERE id = ?", 35));
    args[1] = INT_VAL(999);
    Value result = call_db_fn("row", 2, args);
    ASSERT(IS_NIL(result));
    return true;
}

static bool test_scalar(void) {
    Value args[1];
    args[0] = OBJ_VAL(obj_string_new("SELECT COUNT(*) FROM users", 26));
    Value result = call_db_fn("scalar", 1, args);
    ASSERT(IS_INT(result));
    ASSERT(AS_INT(result) == 2);
    return true;
}

static bool test_scalar_string(void) {
    Value args[2];
    args[0] = OBJ_VAL(obj_string_new("SELECT name FROM users WHERE id = ?", 35));
    args[1] = INT_VAL(2);
    Value result = call_db_fn("scalar", 2, args);
    ASSERT(IS_STRING(result));
    ASSERT(strcmp(AS_STRING(result)->data, "Bob") == 0);
    return true;
}

static bool test_scalar_no_rows(void) {
    Value args[2];
    args[0] = OBJ_VAL(obj_string_new("SELECT name FROM users WHERE id = ?", 35));
    args[1] = INT_VAL(999);
    Value result = call_db_fn("scalar", 2, args);
    ASSERT(IS_NIL(result));
    return true;
}

static bool test_bind_null(void) {
    Value args[2];
    args[0] = OBJ_VAL(obj_string_new("INSERT INTO users (name, age) VALUES (?, ?)", 44));
    args[1] = VAL_NIL;
    // Only provide 1 param for the 2-param query - need both
    Value args2[3];
    args2[0] = args[0];
    args2[1] = OBJ_VAL(obj_string_new("NullAge", 7));
    args2[2] = VAL_NIL;
    Value result = call_db_fn("exec", 3, args2);
    ASSERT(IS_INT(result));

    // Check the null was stored
    Value qargs[2];
    qargs[0] = OBJ_VAL(obj_string_new("SELECT age FROM users WHERE name = ?", 36));
    qargs[1] = OBJ_VAL(obj_string_new("NullAge", 7));
    Value age = call_db_fn("scalar", 2, qargs);
    ASSERT(IS_NIL(age));
    return true;
}

static bool test_bind_float(void) {
    // Create a table with a float column
    Value args[1];
    args[0] = OBJ_VAL(obj_string_new(
        "CREATE TABLE scores (name TEXT, score REAL)", 43));
    call_db_fn("exec", 1, args);

    Value iargs[3];
    iargs[0] = OBJ_VAL(obj_string_new("INSERT INTO scores (name, score) VALUES (?, ?)", 47));
    iargs[1] = OBJ_VAL(obj_string_new("test", 4));
    iargs[2] = FLOAT_VAL(3.14);
    call_db_fn("exec", 3, iargs);

    Value qargs[1];
    qargs[0] = OBJ_VAL(obj_string_new("SELECT score FROM scores WHERE name = 'test'", 44));
    Value result = call_db_fn("scalar", 1, qargs);
    ASSERT(IS_FLOAT(result));
    double val = AS_DOUBLE(result);
    ASSERT(val > 3.13 && val < 3.15);
    return true;
}

static bool test_bind_bool(void) {
    Value args[1];
    args[0] = OBJ_VAL(obj_string_new(
        "CREATE TABLE flags (name TEXT, active INTEGER)", 46));
    call_db_fn("exec", 1, args);

    Value iargs[3];
    iargs[0] = OBJ_VAL(obj_string_new("INSERT INTO flags (name, active) VALUES (?, ?)", 47));
    iargs[1] = OBJ_VAL(obj_string_new("flag1", 5));
    iargs[2] = BOOL_VAL(true);
    call_db_fn("exec", 3, iargs);

    Value qargs[1];
    qargs[0] = OBJ_VAL(obj_string_new("SELECT active FROM flags WHERE name = 'flag1'", 45));
    Value result = call_db_fn("scalar", 1, qargs);
    ASSERT(IS_INT(result));
    ASSERT(AS_INT(result) == 1);
    return true;
}

static bool test_close(void) {
    Value result = call_db_fn("close", 0, NULL);
    ASSERT(IS_NIL(result));

    // After close, queries should return nil
    Value args[1];
    args[0] = OBJ_VAL(obj_string_new("SELECT 1", 8));
    Value qresult = call_db_fn("query", 1, args);
    ASSERT(IS_NIL(qresult));
    return true;
}

int main(void) {
    // Initialize subsystems (same as main.c)
    gc_init();
    intern_table_init();
    heap_init();
    vm_init();
    stdlib_init();

    printf("=== DB (SQLite) Tests ===\n");

    TEST(open_memory);
    TEST(exec_create_table);
    TEST(exec_insert);
    TEST(exec_insert_second);
    TEST(query_all);
    TEST(query_with_params);
    TEST(row_found);
    TEST(row_not_found);
    TEST(scalar);
    TEST(scalar_string);
    TEST(scalar_no_rows);
    TEST(bind_null);
    TEST(bind_float);
    TEST(bind_bool);
    TEST(close);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);

    // Cleanup
    vm_free();
    intern_table_destroy();
    gc_destroy();
    heap_destroy();

    return tests_passed == tests_run ? 0 : 1;
}
