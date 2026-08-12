#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"
#include "sqlite3.h"

// Global database connection
// NOTE: This is process-global mutable state. The current design assumes
// single-request-at-a-time processing. Concurrent or pipelined request
// handling would require per-request context objects instead.
static sqlite3* db_conn = NULL;

// Helper: bind parameters to a prepared statement
// params start at args[1] (args[0] is the SQL string)
static bool bind_params(sqlite3_stmt* stmt, int argc, Value* args) {
    int param_count = sqlite3_bind_parameter_count(stmt);
    int provided = argc - 1; // first arg is SQL

    if (provided < param_count) {
        return false;
    }

    for (int i = 0; i < param_count; i++) {
        Value val = args[i + 1];
        int idx = i + 1; // SQLite params are 1-indexed

        if (IS_NIL(val)) {
            sqlite3_bind_null(stmt, idx);
        } else if (IS_INT(val)) {
            sqlite3_bind_int64(stmt, idx, AS_INT(val));
        } else if (IS_FLOAT(val)) {
            sqlite3_bind_double(stmt, idx, AS_DOUBLE(val));
        } else if (IS_BOOL(val)) {
            sqlite3_bind_int(stmt, idx, AS_BOOL(val) ? 1 : 0);
        } else if (IS_STRING(val)) {
            ObjString* s = AS_STRING(val);
            sqlite3_bind_text(stmt, idx, s->data, (int)s->length, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(stmt, idx);
        }
    }
    return true;
}

// Helper: read a single column value from a result row
static Value read_column(sqlite3_stmt* stmt, int col) {
    int col_type = sqlite3_column_type(stmt, col);
    switch (col_type) {
        case SQLITE_INTEGER:
            return INT_VAL(sqlite3_column_int64(stmt, col));
        case SQLITE_FLOAT:
            return FLOAT_VAL(sqlite3_column_double(stmt, col));
        case SQLITE_TEXT: {
            const char* text = (const char*)sqlite3_column_text(stmt, col);
            int len = sqlite3_column_bytes(stmt, col);
            ObjString* str = obj_string_new(text, (uint32_t)len);
            return OBJ_VAL(str);
        }
        case SQLITE_BLOB: {
            const uint8_t* data = (const uint8_t*)sqlite3_column_blob(stmt, col);
            int len = sqlite3_column_bytes(stmt, col);
            ObjBytes* bytes = obj_bytes_new(data, (uint32_t)len);
            return OBJ_VAL(bytes);
        }
        case SQLITE_NULL:
        default:
            return VAL_NIL;
    }
}

// Helper: build a map from the current row of a statement
static Value build_row_map(sqlite3_stmt* stmt) {
    int col_count = sqlite3_column_count(stmt);
    ObjMap* map = obj_map_new();
    gc_push_root(OBJ_VAL(map));

    for (int i = 0; i < col_count; i++) {
        const char* col_name = sqlite3_column_name(stmt, i);
        ObjString* key = obj_string_new(col_name, (uint32_t)strlen(col_name));
        gc_push_root(OBJ_VAL(key)); // root key before allocating value
        Value val = read_column(stmt, i);
        gc_push_root(val);
        obj_map_set(map, key, val);
        gc_pop_root(); // val
        gc_pop_root(); // key
    }

    gc_pop_root(); // map
    return OBJ_VAL(map);
}

// db.open(path) - open a SQLite database in WAL mode
static Value native_db_open(int argc, Value* args) {
    (void)argc;
    if (!IS_STRING(args[0])) return VAL_NIL;

    // Close existing connection if any
    if (db_conn) {
        sqlite3_close(db_conn);
        db_conn = NULL;
    }

    ObjString* path = AS_STRING(args[0]);
    int rc = sqlite3_open(path->data, &db_conn);
    if (rc != SQLITE_OK) {
        if (db_conn) {
            sqlite3_close(db_conn);
            db_conn = NULL;
        }
        return VAL_NIL;
    }

    // Enable WAL mode
    sqlite3_exec(db_conn, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);

    return VAL_TRUE;
}

// db.query(sql, params...) - returns list of maps
static Value native_db_query(int argc, Value* args) {
    if (argc < 1 || !IS_STRING(args[0])) return VAL_NIL;
    if (!db_conn) return VAL_NIL;

    ObjString* sql = AS_STRING(args[0]);
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_conn, sql->data, (int)sql->length, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return VAL_NIL;
    }

    // Bind parameters
    if (!bind_params(stmt, argc, args)) {
        sqlite3_finalize(stmt);
        return VAL_NIL;
    }

    // Execute and collect rows
    ObjList* results = obj_list_new();
    gc_push_root(OBJ_VAL(results));

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        Value row = build_row_map(stmt);
        gc_push_root(row);
        obj_list_push(results, row);
        gc_pop_root(); // row
    }

    gc_pop_root(); // results
    sqlite3_finalize(stmt);

    return OBJ_VAL(results);
}

// db.row(sql, params...) - returns first row as map or nil
static Value native_db_row(int argc, Value* args) {
    if (argc < 1 || !IS_STRING(args[0])) return VAL_NIL;
    if (!db_conn) return VAL_NIL;

    ObjString* sql = AS_STRING(args[0]);
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_conn, sql->data, (int)sql->length, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return VAL_NIL;
    }

    if (!bind_params(stmt, argc, args)) {
        sqlite3_finalize(stmt);
        return VAL_NIL;
    }

    Value result = VAL_NIL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = build_row_map(stmt);
    }

    sqlite3_finalize(stmt);
    return result;
}

// db.exec(sql, params...) - executes statement, returns last_insert_rowid
static Value native_db_exec(int argc, Value* args) {
    if (argc < 1 || !IS_STRING(args[0])) return VAL_NIL;
    if (!db_conn) return VAL_NIL;

    ObjString* sql = AS_STRING(args[0]);
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_conn, sql->data, (int)sql->length, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return VAL_NIL;
    }

    if (!bind_params(stmt, argc, args)) {
        sqlite3_finalize(stmt);
        return VAL_NIL;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        return VAL_NIL;
    }

    int64_t rowid = sqlite3_last_insert_rowid(db_conn);
    return INT_VAL(rowid);
}

// db.scalar(sql, params...) - returns first column of first row
static Value native_db_scalar(int argc, Value* args) {
    if (argc < 1 || !IS_STRING(args[0])) return VAL_NIL;
    if (!db_conn) return VAL_NIL;

    ObjString* sql = AS_STRING(args[0]);
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_conn, sql->data, (int)sql->length, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return VAL_NIL;
    }

    if (!bind_params(stmt, argc, args)) {
        sqlite3_finalize(stmt);
        return VAL_NIL;
    }

    Value result = VAL_NIL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = read_column(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return result;
}

// db.close() - close the database connection
static Value native_db_close(int argc, Value* args) {
    (void)argc;
    (void)args;
    if (db_conn) {
        sqlite3_close(db_conn);
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
    int rc = sqlite3_exec(db_conn, "BEGIN;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        return VAL_NIL;
    }

    // Call the closure with no arguments
    vm_push(callee);
    Value result = vm_call(callee, 0);

    // Check for error
    if (vm.had_error) {
        sqlite3_exec(db_conn, "ROLLBACK;", NULL, NULL, NULL);
        return VAL_NIL;
    }

    // COMMIT
    rc = sqlite3_exec(db_conn, "COMMIT;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_exec(db_conn, "ROLLBACK;", NULL, NULL, NULL);
        return VAL_NIL;
    }

    return result;
}

void stdlib_db_init(ObjMap* pkg) {
    stdlib_register(pkg, "open", native_db_open, 1);
    stdlib_register(pkg, "query", native_db_query, -1);
    stdlib_register(pkg, "row", native_db_row, -1);
    stdlib_register(pkg, "exec", native_db_exec, -1);
    stdlib_register(pkg, "scalar", native_db_scalar, -1);
    stdlib_register(pkg, "close", native_db_close, 0);
    stdlib_register(pkg, "transaction", native_db_transaction, 1);
}
