#ifndef VEK_TURSO_H
#define VEK_TURSO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Turso/libsql C SDK stub interface.
 * This provides the type definitions and function signatures for the
 * Turso database client. The real libsql C SDK will replace the stub
 * implementation in turso.c when integrated.
 *
 * Connection is configured via environment variables:
 *   TURSO_DATABASE_URL  - the libsql/Turso database URL
 *   TURSO_AUTH_TOKEN    - authentication token
 */

// Error codes
#define TURSO_OK        0
#define TURSO_ERROR     1
#define TURSO_BUSY      2
#define TURSO_NOT_FOUND 3

// Column types
#define TURSO_NULL      0
#define TURSO_INTEGER   1
#define TURSO_FLOAT     2
#define TURSO_TEXT      3
#define TURSO_BLOB      4

// Opaque types
typedef struct turso_conn turso_conn;
typedef struct turso_stmt turso_stmt;
typedef struct turso_rows turso_rows;

// Connection management
// Reads TURSO_DATABASE_URL and TURSO_AUTH_TOKEN from environment.
// Returns NULL on failure (env vars missing or connection failed).
turso_conn* turso_connect(void);
void turso_disconnect(turso_conn* conn);

// Statement preparation
turso_stmt* turso_prepare(turso_conn* conn, const char* sql, int sql_len);
void turso_stmt_free(turso_stmt* stmt);

// Parameter binding (1-indexed positions)
int turso_bind_int(turso_stmt* stmt, int pos, int64_t value);
int turso_bind_text(turso_stmt* stmt, int pos, const char* value, int len);
int turso_bind_null(turso_stmt* stmt, int pos);
int turso_bind_double(turso_stmt* stmt, int pos, double value);

// Execution (for INSERT/UPDATE/DELETE/DDL)
// Returns TURSO_OK on success. Sets *last_id to last insert rowid.
int turso_exec(turso_stmt* stmt, int64_t* last_id);

// Query execution (for SELECT)
// Returns rows object or NULL on error.
turso_rows* turso_query(turso_stmt* stmt);

// Row iteration
int turso_row_count(turso_rows* rows);
int turso_column_count(turso_rows* rows);
const char* turso_column_name(turso_rows* rows, int col);
int turso_column_type(turso_rows* rows, int row, int col);
int64_t turso_row_get_int(turso_rows* rows, int row, int col);
double turso_row_get_double(turso_rows* rows, int row, int col);
const char* turso_row_get_text(turso_rows* rows, int row, int col);
int turso_row_get_text_len(turso_rows* rows, int row, int col);
const uint8_t* turso_row_get_blob(turso_rows* rows, int row, int col, int* len);
void turso_rows_free(turso_rows* rows);

// Transaction support
int turso_begin(turso_conn* conn);
int turso_commit(turso_conn* conn);
int turso_rollback(turso_conn* conn);

// Direct SQL execution (no result needed, convenience wrapper)
int turso_exec_sql(turso_conn* conn, const char* sql);

#endif // VEK_TURSO_H
