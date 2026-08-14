#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"
#include "vek_db.h"

/*
 * db stdlib package - SQLite backend.
 *
 * Connection is configured via environment variable:
 *   DATABASE_PATH - path to the SQLite database file (default: "app.db")
 *
 * NOTE: This is process-global mutable state. The current design assumes
 * single-request-at-a-time processing.
 */

static vek_db_conn* db_conn = NULL;

// Helper: bind parameters to a prepared statement
// params start at args[1] (args[0] is the SQL string)
static bool bind_params(vek_db_stmt* stmt, int argc, Value* args) {
    int provided = argc - 1; // first arg is SQL

    for (int i = 0; i < provided; i++) {
        Value val = args[i + 1];
        int pos = i + 1; // 1-indexed

        if (IS_NIL(val)) {
            vek_db_bind_null(stmt, pos);
        } else if (IS_INT(val)) {
            vek_db_bind_int(stmt, pos, AS_INT(val));
        } else if (IS_FLOAT(val)) {
            vek_db_bind_double(stmt, pos, AS_DOUBLE(val));
        } else if (IS_BOOL(val)) {
            vek_db_bind_int(stmt, pos, AS_BOOL(val) ? 1 : 0);
        } else if (IS_STRING(val)) {
            ObjString* s = AS_STRING(val);
            vek_db_bind_text(stmt, pos, s->data, (int)s->length);
        } else {
            vek_db_bind_null(stmt, pos);
        }
    }
    return true;
}

// Helper: read a single column value from a result row
static Value read_column(vek_db_rows* rows, int row, int col) {
    int col_type = vek_db_column_type(rows, row, col);
    switch (col_type) {
        case VEK_DB_INTEGER:
            return INT_VAL(vek_db_row_get_int(rows, row, col));
        case VEK_DB_FLOAT:
            return FLOAT_VAL(vek_db_row_get_double(rows, row, col));
        case VEK_DB_TEXT: {
            const char* text = vek_db_row_get_text(rows, row, col);
            int len = vek_db_row_get_text_len(rows, row, col);
            if (!text) return VAL_NIL;
            ObjString* str = obj_string_new(text, (uint32_t)len);
            return OBJ_VAL(str);
        }
        case VEK_DB_BLOB: {
            int len = 0;
            const uint8_t* data = vek_db_row_get_blob(rows, row, col, &len);
            if (!data) return VAL_NIL;
            ObjBytes* bytes = obj_bytes_new(data, (uint32_t)len);
            return OBJ_VAL(bytes);
        }
        case VEK_DB_NULL:
        default:
            return VAL_NIL;
    }
}

// Helper: build a map from a row in the result set
static Value build_row_map(vek_db_rows* rows, int row) {
    int col_count = vek_db_column_count(rows);
    ObjMap* map = obj_map_new();
    gc_push_root(OBJ_VAL(map));

    for (int i = 0; i < col_count; i++) {
        const char* col_name = vek_db_column_name(rows, i);
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

// db.connect() - connect to SQLite database
static Value native_db_connect(int argc, Value* args) {
    (void)argc; (void)args;

    // Close existing connection if any
    if (db_conn) {
        vek_db_disconnect(db_conn);
        db_conn = NULL;
    }

    db_conn = vek_db_connect();
    if (!db_conn) return VAL_NIL;

    return VAL_TRUE;
}

// db.query(sql, params...) - returns list of maps
static Value native_db_query(int argc, Value* args) {
    if (argc < 1 || !IS_STRING(args[0])) return VAL_NIL;
    if (!db_conn) return VAL_NIL;

    ObjString* sql = AS_STRING(args[0]);
    vek_db_stmt* stmt = vek_db_prepare(db_conn, sql->data, (int)sql->length);
    if (!stmt) return VAL_NIL;

    // Bind parameters
    if (!bind_params(stmt, argc, args)) {
        vek_db_stmt_free(stmt);
        return VAL_NIL;
    }

    // Execute query
    vek_db_rows* rows = vek_db_query(stmt);
    vek_db_stmt_free(stmt);
    if (!rows) return VAL_NIL;

    // Build result list
    ObjList* results = obj_list_new();
    gc_push_root(OBJ_VAL(results));

    int row_count = vek_db_row_count(rows);
    for (int i = 0; i < row_count; i++) {
        Value row = build_row_map(rows, i);
        gc_push_root(row);
        obj_list_push(results, row);
        gc_pop_root(); // row
    }

    gc_pop_root(); // results
    vek_db_rows_free(rows);

    return OBJ_VAL(results);
}

// db.exec(sql, params...) - executes statement, returns last_insert_rowid
static Value native_db_exec(int argc, Value* args) {
    if (argc < 1 || !IS_STRING(args[0])) return VAL_NIL;
    if (!db_conn) return VAL_NIL;

    ObjString* sql = AS_STRING(args[0]);
    vek_db_stmt* stmt = vek_db_prepare(db_conn, sql->data, (int)sql->length);
    if (!stmt) return VAL_NIL;

    if (!bind_params(stmt, argc, args)) {
        vek_db_stmt_free(stmt);
        return VAL_NIL;
    }

    int64_t last_id = 0;
    int rc = vek_db_exec(stmt, &last_id);
    vek_db_stmt_free(stmt);

    if (rc != VEK_DB_OK) return VAL_NIL;

    return INT_VAL(last_id);
}

// db.close() - close the database connection
static Value native_db_close(int argc, Value* args) {
    (void)argc; (void)args;
    if (db_conn) {
        vek_db_disconnect(db_conn);
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
    int rc = vek_db_begin(db_conn);
    if (rc != VEK_DB_OK) return VAL_NIL;

    // Call the closure with no arguments
    vm_push(callee);
    Value result = vm_call(callee, 0);

    // Check for error
    if (vm.had_error) {
        vek_db_rollback(db_conn);
        return VAL_NIL;
    }

    // COMMIT
    rc = vek_db_commit(db_conn);
    if (rc != VEK_DB_OK) {
        vek_db_rollback(db_conn);
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
