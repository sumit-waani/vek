#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"
#include "turso.h"

/*
 * db stdlib package - Turso/libsql backend.
 *
 * Connection is configured via environment variables:
 *   TURSO_DATABASE_URL  - the libsql/Turso database URL
 *   TURSO_AUTH_TOKEN    - authentication token
 *
 * NOTE: This is process-global mutable state. The current design assumes
 * single-request-at-a-time processing.
 */

static turso_conn* db_conn = NULL;

// Helper: bind parameters to a prepared statement
// params start at args[1] (args[0] is the SQL string)
static bool bind_params(turso_stmt* stmt, int argc, Value* args) {
    int provided = argc - 1; // first arg is SQL

    for (int i = 0; i < provided; i++) {
        Value val = args[i + 1];
        int pos = i + 1; // 1-indexed

        if (IS_NIL(val)) {
            turso_bind_null(stmt, pos);
        } else if (IS_INT(val)) {
            turso_bind_int(stmt, pos, AS_INT(val));
        } else if (IS_FLOAT(val)) {
            turso_bind_double(stmt, pos, AS_DOUBLE(val));
        } else if (IS_BOOL(val)) {
            turso_bind_int(stmt, pos, AS_BOOL(val) ? 1 : 0);
        } else if (IS_STRING(val)) {
            ObjString* s = AS_STRING(val);
            turso_bind_text(stmt, pos, s->data, (int)s->length);
        } else {
            turso_bind_null(stmt, pos);
        }
    }
    return true;
}

// Helper: read a single column value from a result row
static Value read_column(turso_rows* rows, int row, int col) {
    int col_type = turso_column_type(rows, row, col);
    switch (col_type) {
        case TURSO_INTEGER:
            return INT_VAL(turso_row_get_int(rows, row, col));
        case TURSO_FLOAT:
            return FLOAT_VAL(turso_row_get_double(rows, row, col));
        case TURSO_TEXT: {
            const char* text = turso_row_get_text(rows, row, col);
            int len = turso_row_get_text_len(rows, row, col);
            if (!text) return VAL_NIL;
            ObjString* str = obj_string_new(text, (uint32_t)len);
            return OBJ_VAL(str);
        }
        case TURSO_BLOB: {
            int len = 0;
            const uint8_t* data = turso_row_get_blob(rows, row, col, &len);
            if (!data) return VAL_NIL;
            ObjBytes* bytes = obj_bytes_new(data, (uint32_t)len);
            return OBJ_VAL(bytes);
        }
        case TURSO_NULL:
        default:
            return VAL_NIL;
    }
}

// Helper: build a map from a row in the result set
static Value build_row_map(turso_rows* rows, int row) {
    int col_count = turso_column_count(rows);
    ObjMap* map = obj_map_new();
    gc_push_root(OBJ_VAL(map));

    for (int i = 0; i < col_count; i++) {
        const char* col_name = turso_column_name(rows, i);
        if (!col_name) continue;
        ObjString* key = obj_string_new(col_name, (uint32_t)strlen(col_name));
        gc_push_root(OBJ_VAL(key));
        Value val = read_column(rows, row, i);
        gc_push_root(val);
        obj_map_set(map, key, val);
        gc_pop_root(); // val
        gc_pop_root(); // key
    }

    gc_pop_root(); // map
    return OBJ_VAL(map);
}

// db.connect() - connect to Turso using env vars
static Value native_db_connect(int argc, Value* args) {
    (void)argc; (void)args;

    // Close existing connection if any
    if (db_conn) {
        turso_disconnect(db_conn);
        db_conn = NULL;
    }

    db_conn = turso_connect();
    if (!db_conn) return VAL_NIL;

    return VAL_TRUE;
}

// db.query(sql, params...) - returns list of maps
static Value native_db_query(int argc, Value* args) {
    if (argc < 1 || !IS_STRING(args[0])) return VAL_NIL;
    if (!db_conn) return VAL_NIL;

    ObjString* sql = AS_STRING(args[0]);
    turso_stmt* stmt = turso_prepare(db_conn, sql->data, (int)sql->length);
    if (!stmt) return VAL_NIL;

    // Bind parameters
    if (!bind_params(stmt, argc, args)) {
        turso_stmt_free(stmt);
        return VAL_NIL;
    }

    // Execute query
    turso_rows* rows = turso_query(stmt);
    turso_stmt_free(stmt);
    if (!rows) return VAL_NIL;

    // Build result list
    ObjList* results = obj_list_new();
    gc_push_root(OBJ_VAL(results));

    int row_count = turso_row_count(rows);
    for (int i = 0; i < row_count; i++) {
        Value row = build_row_map(rows, i);
        gc_push_root(row);
        obj_list_push(results, row);
        gc_pop_root(); // row
    }

    gc_pop_root(); // results
    turso_rows_free(rows);

    return OBJ_VAL(results);
}

// db.exec(sql, params...) - executes statement, returns last_insert_rowid
static Value native_db_exec(int argc, Value* args) {
    if (argc < 1 || !IS_STRING(args[0])) return VAL_NIL;
    if (!db_conn) return VAL_NIL;

    ObjString* sql = AS_STRING(args[0]);
    turso_stmt* stmt = turso_prepare(db_conn, sql->data, (int)sql->length);
    if (!stmt) return VAL_NIL;

    if (!bind_params(stmt, argc, args)) {
        turso_stmt_free(stmt);
        return VAL_NIL;
    }

    int64_t last_id = 0;
    int rc = turso_exec(stmt, &last_id);
    turso_stmt_free(stmt);

    if (rc != TURSO_OK) return VAL_NIL;

    return INT_VAL(last_id);
}

// db.close() - close the database connection
static Value native_db_close(int argc, Value* args) {
    (void)argc; (void)args;
    if (db_conn) {
        turso_disconnect(db_conn);
        db_conn = NULL;
    }
    return VAL_NIL;
}

// db.transaction(closure) - BEGIN, call closure, COMMIT or ROLLBACK on error
static Value native_db_transaction(int argc, Value* args) {
    (void)argc;
    if (!db_conn) return VAL_NIL;

    Value callee = args[0];
    if (!IS_CLOSURE(callee) && !IS_NATIVE(callee)) {
        return VAL_NIL;
    }

    // BEGIN transaction
    int rc = turso_begin(db_conn);
    if (rc != TURSO_OK) return VAL_NIL;

    // Call the closure with no arguments
    vm_push(callee);
    Value result = vm_call(callee, 0);

    // Check for error
    if (vm.had_error) {
        turso_rollback(db_conn);
        return VAL_NIL;
    }

    // COMMIT
    rc = turso_commit(db_conn);
    if (rc != TURSO_OK) {
        turso_rollback(db_conn);
        return VAL_NIL;
    }

    return result;
}

void stdlib_db_init(ObjMap* pkg) {
    stdlib_register(pkg, "connect", native_db_connect, 0);
    stdlib_register(pkg, "query", native_db_query, -1);
    stdlib_register(pkg, "exec", native_db_exec, -1);
    stdlib_register(pkg, "close", native_db_close, 0);
    stdlib_register(pkg, "transaction", native_db_transaction, 1);
}
