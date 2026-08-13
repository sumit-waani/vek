#define _POSIX_C_SOURCE 200809L
#include "turso.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * Turso/libsql C SDK - Stub Implementation
 *
 * This is a placeholder that provides the correct interface. When env vars
 * are not set, functions fail gracefully. When env vars ARE set, this stub
 * simulates successful connection but operations are no-ops.
 *
 * The real libsql C SDK will replace this implementation.
 */

#define MAX_PARAMS 64
#define MAX_COLUMNS 64
#define MAX_ROWS 256

typedef struct {
    int type;
    int64_t int_val;
    double float_val;
    char* text_val;
    int text_len;
} turso_param;

typedef struct {
    int type;
    int64_t int_val;
    double float_val;
    char* text_val;
    int text_len;
    uint8_t* blob_val;
    int blob_len;
} turso_cell;

typedef struct {
    turso_cell cells[MAX_COLUMNS];
} turso_row_data;

struct turso_conn {
    char* url;
    char* token;
    bool connected;
    bool in_transaction;
};

struct turso_stmt {
    turso_conn* conn;
    char* sql;
    int sql_len;
    turso_param params[MAX_PARAMS];
    int param_count;
};

struct turso_rows {
    int row_count;
    int col_count;
    char* col_names[MAX_COLUMNS];
    turso_row_data rows[MAX_ROWS];
};

turso_conn* turso_connect(void) {
    const char* url = getenv("TURSO_DATABASE_URL");
    const char* token = getenv("TURSO_AUTH_TOKEN");

    if (!url || !token || url[0] == '\0' || token[0] == '\0') {
        return NULL;
    }

    turso_conn* conn = (turso_conn*)calloc(1, sizeof(turso_conn));
    if (!conn) return NULL;

    conn->url = strdup(url);
    conn->token = strdup(token);
    conn->connected = true;
    conn->in_transaction = false;

    return conn;
}

void turso_disconnect(turso_conn* conn) {
    if (!conn) return;
    free(conn->url);
    free(conn->token);
    conn->connected = false;
    free(conn);
}

turso_stmt* turso_prepare(turso_conn* conn, const char* sql, int sql_len) {
    if (!conn || !conn->connected || !sql) return NULL;

    turso_stmt* stmt = (turso_stmt*)calloc(1, sizeof(turso_stmt));
    if (!stmt) return NULL;

    stmt->conn = conn;
    stmt->sql_len = sql_len;
    stmt->sql = (char*)malloc((size_t)sql_len + 1);
    if (!stmt->sql) {
        free(stmt);
        return NULL;
    }
    memcpy(stmt->sql, sql, (size_t)sql_len);
    stmt->sql[sql_len] = '\0';
    stmt->param_count = 0;

    return stmt;
}

void turso_stmt_free(turso_stmt* stmt) {
    if (!stmt) return;
    free(stmt->sql);
    for (int i = 0; i < stmt->param_count; i++) {
        if (stmt->params[i].type == TURSO_TEXT && stmt->params[i].text_val) {
            free(stmt->params[i].text_val);
        }
    }
    free(stmt);
}

int turso_bind_int(turso_stmt* stmt, int pos, int64_t value) {
    if (!stmt || pos < 1 || pos > MAX_PARAMS) return TURSO_ERROR;
    int idx = pos - 1;
    stmt->params[idx].type = TURSO_INTEGER;
    stmt->params[idx].int_val = value;
    if (pos > stmt->param_count) stmt->param_count = pos;
    return TURSO_OK;
}

int turso_bind_text(turso_stmt* stmt, int pos, const char* value, int len) {
    if (!stmt || pos < 1 || pos > MAX_PARAMS) return TURSO_ERROR;
    int idx = pos - 1;
    stmt->params[idx].type = TURSO_TEXT;
    stmt->params[idx].text_val = (char*)malloc((size_t)len + 1);
    if (!stmt->params[idx].text_val) return TURSO_ERROR;
    memcpy(stmt->params[idx].text_val, value, (size_t)len);
    stmt->params[idx].text_val[len] = '\0';
    stmt->params[idx].text_len = len;
    if (pos > stmt->param_count) stmt->param_count = pos;
    return TURSO_OK;
}

int turso_bind_null(turso_stmt* stmt, int pos) {
    if (!stmt || pos < 1 || pos > MAX_PARAMS) return TURSO_ERROR;
    int idx = pos - 1;
    stmt->params[idx].type = TURSO_NULL;
    if (pos > stmt->param_count) stmt->param_count = pos;
    return TURSO_OK;
}

int turso_bind_double(turso_stmt* stmt, int pos, double value) {
    if (!stmt || pos < 1 || pos > MAX_PARAMS) return TURSO_ERROR;
    int idx = pos - 1;
    stmt->params[idx].type = TURSO_FLOAT;
    stmt->params[idx].float_val = value;
    if (pos > stmt->param_count) stmt->param_count = pos;
    return TURSO_OK;
}

int turso_exec(turso_stmt* stmt, int64_t* last_id) {
    if (!stmt || !stmt->conn || !stmt->conn->connected) return TURSO_ERROR;
    if (last_id) *last_id = 0;
    return TURSO_OK;
}

turso_rows* turso_query(turso_stmt* stmt) {
    if (!stmt || !stmt->conn || !stmt->conn->connected) return NULL;
    turso_rows* rows = (turso_rows*)calloc(1, sizeof(turso_rows));
    if (!rows) return NULL;
    rows->row_count = 0;
    rows->col_count = 0;
    return rows;
}

int turso_row_count(turso_rows* rows) {
    if (!rows) return 0;
    return rows->row_count;
}

int turso_column_count(turso_rows* rows) {
    if (!rows) return 0;
    return rows->col_count;
}

const char* turso_column_name(turso_rows* rows, int col) {
    if (!rows || col < 0 || col >= rows->col_count) return NULL;
    return rows->col_names[col];
}

int turso_column_type(turso_rows* rows, int row, int col) {
    if (!rows || row < 0 || row >= rows->row_count || col < 0 || col >= rows->col_count)
        return TURSO_NULL;
    return rows->rows[row].cells[col].type;
}

int64_t turso_row_get_int(turso_rows* rows, int row, int col) {
    if (!rows || row < 0 || row >= rows->row_count || col < 0 || col >= rows->col_count)
        return 0;
    return rows->rows[row].cells[col].int_val;
}

double turso_row_get_double(turso_rows* rows, int row, int col) {
    if (!rows || row < 0 || row >= rows->row_count || col < 0 || col >= rows->col_count)
        return 0.0;
    return rows->rows[row].cells[col].float_val;
}

const char* turso_row_get_text(turso_rows* rows, int row, int col) {
    if (!rows || row < 0 || row >= rows->row_count || col < 0 || col >= rows->col_count)
        return NULL;
    return rows->rows[row].cells[col].text_val;
}

int turso_row_get_text_len(turso_rows* rows, int row, int col) {
    if (!rows || row < 0 || row >= rows->row_count || col < 0 || col >= rows->col_count)
        return 0;
    return rows->rows[row].cells[col].text_len;
}

const uint8_t* turso_row_get_blob(turso_rows* rows, int row, int col, int* len) {
    if (!rows || row < 0 || row >= rows->row_count || col < 0 || col >= rows->col_count) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) *len = rows->rows[row].cells[col].blob_len;
    return rows->rows[row].cells[col].blob_val;
}

void turso_rows_free(turso_rows* rows) {
    if (!rows) return;
    for (int i = 0; i < rows->col_count; i++) {
        free(rows->col_names[i]);
    }
    for (int r = 0; r < rows->row_count; r++) {
        for (int c = 0; c < rows->col_count; c++) {
            turso_cell* cell = &rows->rows[r].cells[c];
            if (cell->type == TURSO_TEXT) free(cell->text_val);
            if (cell->type == TURSO_BLOB) free(cell->blob_val);
        }
    }
    free(rows);
}

int turso_begin(turso_conn* conn) {
    if (!conn || !conn->connected) return TURSO_ERROR;
    if (conn->in_transaction) return TURSO_ERROR;
    conn->in_transaction = true;
    return TURSO_OK;
}

int turso_commit(turso_conn* conn) {
    if (!conn || !conn->connected) return TURSO_ERROR;
    if (!conn->in_transaction) return TURSO_ERROR;
    conn->in_transaction = false;
    return TURSO_OK;
}

int turso_rollback(turso_conn* conn) {
    if (!conn || !conn->connected) return TURSO_ERROR;
    if (!conn->in_transaction) return TURSO_ERROR;
    conn->in_transaction = false;
    return TURSO_OK;
}

int turso_exec_sql(turso_conn* conn, const char* sql) {
    if (!conn || !conn->connected || !sql) return TURSO_ERROR;
    turso_stmt* stmt = turso_prepare(conn, sql, (int)strlen(sql));
    if (!stmt) return TURSO_ERROR;
    int rc = turso_exec(stmt, NULL);
    turso_stmt_free(stmt);
    return rc;
}
