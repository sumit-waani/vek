#define _POSIX_C_SOURCE 200809L
#include "vek_db.h"
#include "sqlite3.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * vek_db - SQLite3 database implementation.
 *
 * Uses the sqlite3 amalgamation compiled into the binary.
 * Connection reads DATABASE_PATH env var (defaults to "app.db").
 */

#define MAX_COLUMNS 64
#define MAX_ROWS 4096

typedef struct {
    int type;
    int64_t int_val;
    double float_val;
    char* text_val;
    int text_len;
    uint8_t* blob_val;
    int blob_len;
} vek_db_cell;

typedef struct {
    vek_db_cell cells[MAX_COLUMNS];
} vek_db_row_data;

struct vek_db_conn {
    sqlite3* db;
    bool in_transaction;
};

struct vek_db_stmt {
    vek_db_conn* conn;
    sqlite3_stmt* sqlite_stmt;
    char* sql;
    int sql_len;
};

struct vek_db_rows {
    int row_count;
    int col_count;
    char* col_names[MAX_COLUMNS];
    vek_db_row_data* rows;
    int rows_capacity;
};

vek_db_conn* vek_db_connect(void) {
    const char* path = getenv("DATABASE_PATH");
    if (!path || path[0] == '\0') {
        path = "app.db";
    }

    sqlite3* db = NULL;
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    int rc = sqlite3_open_v2(path, &db, flags, NULL);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }

    /* Enable WAL mode for better concurrency */
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);

    /* Enable foreign keys */
    sqlite3_exec(db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);

    vek_db_conn* conn = (vek_db_conn*)calloc(1, sizeof(vek_db_conn));
    if (!conn) {
        sqlite3_close(db);
        return NULL;
    }

    conn->db = db;
    conn->in_transaction = false;
    return conn;
}

void vek_db_disconnect(vek_db_conn* conn) {
    if (!conn) return;
    if (conn->db) {
        sqlite3_close(conn->db);
    }
    free(conn);
}

vek_db_stmt* vek_db_prepare(vek_db_conn* conn, const char* sql, int sql_len) {
    if (!conn || !conn->db || !sql) return NULL;

    sqlite3_stmt* sqlite_stmt = NULL;
    int rc = sqlite3_prepare_v2(conn->db, sql, sql_len, &sqlite_stmt, NULL);
    if (rc != SQLITE_OK || !sqlite_stmt) {
        return NULL;
    }

    vek_db_stmt* stmt = (vek_db_stmt*)calloc(1, sizeof(vek_db_stmt));
    if (!stmt) {
        sqlite3_finalize(sqlite_stmt);
        return NULL;
    }

    stmt->conn = conn;
    stmt->sqlite_stmt = sqlite_stmt;
    stmt->sql_len = sql_len;
    stmt->sql = (char*)malloc((size_t)sql_len + 1);
    if (!stmt->sql) {
        sqlite3_finalize(sqlite_stmt);
        free(stmt);
        return NULL;
    }
    memcpy(stmt->sql, sql, (size_t)sql_len);
    stmt->sql[sql_len] = '\0';

    return stmt;
}

void vek_db_stmt_free(vek_db_stmt* stmt) {
    if (!stmt) return;
    if (stmt->sqlite_stmt) {
        sqlite3_finalize(stmt->sqlite_stmt);
    }
    free(stmt->sql);
    free(stmt);
}

int vek_db_bind_int(vek_db_stmt* stmt, int pos, int64_t value) {
    if (!stmt || !stmt->sqlite_stmt) return VEK_DB_ERROR;
    int rc = sqlite3_bind_int64(stmt->sqlite_stmt, pos, value);
    return (rc == SQLITE_OK) ? VEK_DB_OK : VEK_DB_ERROR;
}

int vek_db_bind_text(vek_db_stmt* stmt, int pos, const char* value, int len) {
    if (!stmt || !stmt->sqlite_stmt) return VEK_DB_ERROR;
    int rc = sqlite3_bind_text(stmt->sqlite_stmt, pos, value, len, SQLITE_TRANSIENT);
    return (rc == SQLITE_OK) ? VEK_DB_OK : VEK_DB_ERROR;
}

int vek_db_bind_null(vek_db_stmt* stmt, int pos) {
    if (!stmt || !stmt->sqlite_stmt) return VEK_DB_ERROR;
    int rc = sqlite3_bind_null(stmt->sqlite_stmt, pos);
    return (rc == SQLITE_OK) ? VEK_DB_OK : VEK_DB_ERROR;
}

int vek_db_bind_double(vek_db_stmt* stmt, int pos, double value) {
    if (!stmt || !stmt->sqlite_stmt) return VEK_DB_ERROR;
    int rc = sqlite3_bind_double(stmt->sqlite_stmt, pos, value);
    return (rc == SQLITE_OK) ? VEK_DB_OK : VEK_DB_ERROR;
}

int vek_db_exec(vek_db_stmt* stmt, int64_t* last_id) {
    if (!stmt || !stmt->sqlite_stmt || !stmt->conn) return VEK_DB_ERROR;

    int rc = sqlite3_step(stmt->sqlite_stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        if (rc == SQLITE_BUSY) return VEK_DB_BUSY;
        return VEK_DB_ERROR;
    }

    if (last_id) {
        *last_id = sqlite3_last_insert_rowid(stmt->conn->db);
    }

    return VEK_DB_OK;
}

static int sqlite_type_to_vek(int sqlite_type) {
    switch (sqlite_type) {
        case SQLITE_INTEGER: return VEK_DB_INTEGER;
        case SQLITE_FLOAT:   return VEK_DB_FLOAT;
        case SQLITE_TEXT:    return VEK_DB_TEXT;
        case SQLITE_BLOB:    return VEK_DB_BLOB;
        case SQLITE_NULL:
        default:             return VEK_DB_NULL;
    }
}

vek_db_rows* vek_db_query(vek_db_stmt* stmt) {
    if (!stmt || !stmt->sqlite_stmt || !stmt->conn) return NULL;

    vek_db_rows* rows = (vek_db_rows*)calloc(1, sizeof(vek_db_rows));
    if (!rows) return NULL;

    int col_count = sqlite3_column_count(stmt->sqlite_stmt);
    if (col_count > MAX_COLUMNS) col_count = MAX_COLUMNS;
    rows->col_count = col_count;

    /* Copy column names */
    for (int i = 0; i < col_count; i++) {
        const char* name = sqlite3_column_name(stmt->sqlite_stmt, i);
        rows->col_names[i] = name ? strdup(name) : strdup("");
    }

    /* Initial allocation for rows */
    rows->rows_capacity = 64;
    rows->rows = (vek_db_row_data*)calloc((size_t)rows->rows_capacity, sizeof(vek_db_row_data));
    if (!rows->rows) {
        for (int i = 0; i < col_count; i++) free(rows->col_names[i]);
        free(rows);
        return NULL;
    }

    rows->row_count = 0;

    /* Step through all rows */
    int rc;
    while ((rc = sqlite3_step(stmt->sqlite_stmt)) == SQLITE_ROW) {
        if (rows->row_count >= rows->rows_capacity) {
            int new_cap = rows->rows_capacity * 2;
            vek_db_row_data* new_rows = (vek_db_row_data*)realloc(
                rows->rows, (size_t)new_cap * sizeof(vek_db_row_data));
            if (!new_rows) break;
            memset(new_rows + rows->rows_capacity, 0,
                   (size_t)(new_cap - rows->rows_capacity) * sizeof(vek_db_row_data));
            rows->rows = new_rows;
            rows->rows_capacity = new_cap;
        }

        int r = rows->row_count;
        for (int c = 0; c < col_count; c++) {
            vek_db_cell* cell = &rows->rows[r].cells[c];
            int stype = sqlite3_column_type(stmt->sqlite_stmt, c);
            cell->type = sqlite_type_to_vek(stype);

            switch (stype) {
                case SQLITE_INTEGER:
                    cell->int_val = sqlite3_column_int64(stmt->sqlite_stmt, c);
                    break;
                case SQLITE_FLOAT:
                    cell->float_val = sqlite3_column_double(stmt->sqlite_stmt, c);
                    break;
                case SQLITE_TEXT: {
                    const char* text = (const char*)sqlite3_column_text(stmt->sqlite_stmt, c);
                    int len = sqlite3_column_bytes(stmt->sqlite_stmt, c);
                    if (text) {
                        cell->text_val = (char*)malloc((size_t)len + 1);
                        if (cell->text_val) {
                            memcpy(cell->text_val, text, (size_t)len);
                            cell->text_val[len] = '\0';
                        }
                        cell->text_len = len;
                    }
                    break;
                }
                case SQLITE_BLOB: {
                    const void* data = sqlite3_column_blob(stmt->sqlite_stmt, c);
                    int len = sqlite3_column_bytes(stmt->sqlite_stmt, c);
                    if (data && len > 0) {
                        cell->blob_val = (uint8_t*)malloc((size_t)len);
                        if (cell->blob_val) {
                            memcpy(cell->blob_val, data, (size_t)len);
                        }
                        cell->blob_len = len;
                    }
                    break;
                }
                default:
                    break;
            }
        }
        rows->row_count++;
    }

    return rows;
}

int vek_db_row_count(vek_db_rows* rows) {
    if (!rows) return 0;
    return rows->row_count;
}

int vek_db_column_count(vek_db_rows* rows) {
    if (!rows) return 0;
    return rows->col_count;
}

const char* vek_db_column_name(vek_db_rows* rows, int col) {
    if (!rows || col < 0 || col >= rows->col_count) return NULL;
    return rows->col_names[col];
}

int vek_db_column_type(vek_db_rows* rows, int row, int col) {
    if (!rows || row < 0 || row >= rows->row_count || col < 0 || col >= rows->col_count)
        return VEK_DB_NULL;
    return rows->rows[row].cells[col].type;
}

int64_t vek_db_row_get_int(vek_db_rows* rows, int row, int col) {
    if (!rows || row < 0 || row >= rows->row_count || col < 0 || col >= rows->col_count)
        return 0;
    return rows->rows[row].cells[col].int_val;
}

double vek_db_row_get_double(vek_db_rows* rows, int row, int col) {
    if (!rows || row < 0 || row >= rows->row_count || col < 0 || col >= rows->col_count)
        return 0.0;
    return rows->rows[row].cells[col].float_val;
}

const char* vek_db_row_get_text(vek_db_rows* rows, int row, int col) {
    if (!rows || row < 0 || row >= rows->row_count || col < 0 || col >= rows->col_count)
        return NULL;
    return rows->rows[row].cells[col].text_val;
}

int vek_db_row_get_text_len(vek_db_rows* rows, int row, int col) {
    if (!rows || row < 0 || row >= rows->row_count || col < 0 || col >= rows->col_count)
        return 0;
    return rows->rows[row].cells[col].text_len;
}

const uint8_t* vek_db_row_get_blob(vek_db_rows* rows, int row, int col, int* len) {
    if (!rows || row < 0 || row >= rows->row_count || col < 0 || col >= rows->col_count) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) *len = rows->rows[row].cells[col].blob_len;
    return rows->rows[row].cells[col].blob_val;
}

void vek_db_rows_free(vek_db_rows* rows) {
    if (!rows) return;
    for (int i = 0; i < rows->col_count; i++) {
        free(rows->col_names[i]);
    }
    for (int r = 0; r < rows->row_count; r++) {
        for (int c = 0; c < rows->col_count; c++) {
            vek_db_cell* cell = &rows->rows[r].cells[c];
            if (cell->type == VEK_DB_TEXT) free(cell->text_val);
            if (cell->type == VEK_DB_BLOB) free(cell->blob_val);
        }
    }
    free(rows->rows);
    free(rows);
}

int vek_db_begin(vek_db_conn* conn) {
    if (!conn || !conn->db) return VEK_DB_ERROR;
    if (conn->in_transaction) return VEK_DB_ERROR;

    int rc = sqlite3_exec(conn->db, "BEGIN", NULL, NULL, NULL);
    if (rc != SQLITE_OK) return VEK_DB_ERROR;

    conn->in_transaction = true;
    return VEK_DB_OK;
}

int vek_db_commit(vek_db_conn* conn) {
    if (!conn || !conn->db) return VEK_DB_ERROR;
    if (!conn->in_transaction) return VEK_DB_ERROR;

    int rc = sqlite3_exec(conn->db, "COMMIT", NULL, NULL, NULL);
    conn->in_transaction = false;
    if (rc != SQLITE_OK) return VEK_DB_ERROR;

    return VEK_DB_OK;
}

int vek_db_rollback(vek_db_conn* conn) {
    if (!conn || !conn->db) return VEK_DB_ERROR;
    if (!conn->in_transaction) return VEK_DB_ERROR;

    int rc = sqlite3_exec(conn->db, "ROLLBACK", NULL, NULL, NULL);
    conn->in_transaction = false;
    if (rc != SQLITE_OK) return VEK_DB_ERROR;

    return VEK_DB_OK;
}

int vek_db_exec_sql(vek_db_conn* conn, const char* sql) {
    if (!conn || !conn->db || !sql) return VEK_DB_ERROR;

    char* errmsg = NULL;
    int rc = sqlite3_exec(conn->db, sql, NULL, NULL, &errmsg);
    if (errmsg) sqlite3_free(errmsg);

    if (rc != SQLITE_OK) {
        if (rc == SQLITE_BUSY) return VEK_DB_BUSY;
        return VEK_DB_ERROR;
    }
    return VEK_DB_OK;
}
