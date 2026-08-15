#ifndef VEK_DB_H
#define VEK_DB_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * vek_db - SQLite3 database interface.
 *
 * Connection is configured via environment variable:
 *   DATABASE_PATH - path to the SQLite database file (default: "app.db")
 *
 * Uses the SQLite3 amalgamation compiled directly into the binary.
 */

// Error codes
#define VEK_DB_OK        0
#define VEK_DB_ERROR     1
#define VEK_DB_BUSY      2
#define VEK_DB_NOT_FOUND 3

// Column types
#define VEK_DB_NULL      0
#define VEK_DB_INTEGER   1
#define VEK_DB_FLOAT     2
#define VEK_DB_TEXT      3
#define VEK_DB_BLOB      4

// Opaque types
typedef struct vek_db_conn vek_db_conn;
typedef struct vek_db_stmt vek_db_stmt;
typedef struct vek_db_rows vek_db_rows;

// Connection management
// Reads DATABASE_PATH from environment (defaults to "app.db").
// Returns NULL on failure.
vek_db_conn* vek_db_connect(void);
void vek_db_disconnect(vek_db_conn* conn);

// Statement preparation
vek_db_stmt* vek_db_prepare(vek_db_conn* conn, const char* sql, int sql_len);
void vek_db_stmt_free(vek_db_stmt* stmt);

// Parameter binding (1-indexed positions)
int vek_db_bind_int(vek_db_stmt* stmt, int pos, int64_t value);
int vek_db_bind_text(vek_db_stmt* stmt, int pos, const char* value, int len);
int vek_db_bind_null(vek_db_stmt* stmt, int pos);
int vek_db_bind_double(vek_db_stmt* stmt, int pos, double value);

// Execution (for INSERT/UPDATE/DELETE/DDL)
// Returns VEK_DB_OK on success. Sets *last_id to last insert rowid.
int vek_db_exec(vek_db_stmt* stmt, int64_t* last_id);

// Query execution (for SELECT)
// Returns rows object or NULL on error.
vek_db_rows* vek_db_query(vek_db_stmt* stmt);

// Row iteration
int vek_db_row_count(vek_db_rows* rows);
int vek_db_column_count(vek_db_rows* rows);
const char* vek_db_column_name(vek_db_rows* rows, int col);
int vek_db_column_type(vek_db_rows* rows, int row, int col);
int64_t vek_db_row_get_int(vek_db_rows* rows, int row, int col);
double vek_db_row_get_double(vek_db_rows* rows, int row, int col);
const char* vek_db_row_get_text(vek_db_rows* rows, int row, int col);
int vek_db_row_get_text_len(vek_db_rows* rows, int row, int col);
const uint8_t* vek_db_row_get_blob(vek_db_rows* rows, int row, int col, int* len);
void vek_db_rows_free(vek_db_rows* rows);

// Transaction support
int vek_db_begin(vek_db_conn* conn);
int vek_db_commit(vek_db_conn* conn);
int vek_db_rollback(vek_db_conn* conn);

// Direct SQL execution (no result needed, convenience wrapper)
int vek_db_exec_sql(vek_db_conn* conn, const char* sql);

#endif // VEK_DB_H
