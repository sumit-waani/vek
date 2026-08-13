#define _GNU_SOURCE

#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"
#include "turso.h"

#include <time.h>

/*
 * Jobs stdlib package - Turso/libsql backend.
 *
 * Uses the same TURSO_DATABASE_URL / TURSO_AUTH_TOKEN connection.
 * Table schema: _vek_jobs (id, name, args, status, attempts, run_at, created_at)
 */

static turso_conn* jobs_db = NULL;
static bool table_created = false;

#define MAX_HANDLERS 64
typedef struct {
    char name[128];
    Value closure;
} JobHandler;

static JobHandler handlers[MAX_HANDLERS];
static int handler_count = 0;

// ---- Simple JSON encoder for ObjMap (inline) ----

static void jobs_json_append(char** buf, size_t* len, size_t* cap, const char* s, size_t slen) {
    while (*len + slen + 1 > *cap) {
        *cap = (*cap < 64) ? 64 : *cap * 2;
        *buf = (char*)realloc(*buf, *cap);
    }
    memcpy(*buf + *len, s, slen);
    *len += slen;
    (*buf)[*len] = '\0';
}

static void jobs_json_char(char** buf, size_t* len, size_t* cap, char c) {
    jobs_json_append(buf, len, cap, &c, 1);
}

static void jobs_json_string(const char* s, size_t slen, char** buf, size_t* len, size_t* cap) {
    jobs_json_char(buf, len, cap, '"');
    for (size_t i = 0; i < slen; i++) {
        char c = s[i];
        switch (c) {
            case '"':  jobs_json_append(buf, len, cap, "\\\"", 2); break;
            case '\\': jobs_json_append(buf, len, cap, "\\\\", 2); break;
            case '\n': jobs_json_append(buf, len, cap, "\\n", 2); break;
            case '\r': jobs_json_append(buf, len, cap, "\\r", 2); break;
            case '\t': jobs_json_append(buf, len, cap, "\\t", 2); break;
            default:   jobs_json_char(buf, len, cap, c); break;
        }
    }
    jobs_json_char(buf, len, cap, '"');
}

static void jobs_json_value(Value val, char** buf, size_t* len, size_t* cap) {
    if (IS_NIL(val)) {
        jobs_json_append(buf, len, cap, "null", 4);
    } else if (IS_BOOL(val)) {
        if (AS_BOOL(val)) {
            jobs_json_append(buf, len, cap, "true", 4);
        } else {
            jobs_json_append(buf, len, cap, "false", 5);
        }
    } else if (IS_INT(val)) {
        char num[32];
        int n = snprintf(num, sizeof(num), "%lld", (long long)AS_INT(val));
        jobs_json_append(buf, len, cap, num, (size_t)n);
    } else if (IS_FLOAT(val)) {
        char num[64];
        int n = snprintf(num, sizeof(num), "%g", AS_DOUBLE(val));
        jobs_json_append(buf, len, cap, num, (size_t)n);
    } else if (IS_STRING(val)) {
        ObjString* s = AS_STRING(val);
        jobs_json_string(s->data, s->length, buf, len, cap);
    } else if (IS_MAP(val)) {
        ObjMap* map = AS_MAP(val);
        jobs_json_char(buf, len, cap, '{');
        bool first = true;
        for (uint32_t i = 0; i < map->capacity; i++) {
            if (map->entries[i].key == NULL || map->entries[i].key == MAP_TOMBSTONE) continue;
            if (!first) jobs_json_char(buf, len, cap, ',');
            first = false;
            jobs_json_string(map->entries[i].key->data, map->entries[i].key->length, buf, len, cap);
            jobs_json_char(buf, len, cap, ':');
            jobs_json_value(map->entries[i].value, buf, len, cap);
        }
        jobs_json_char(buf, len, cap, '}');
    } else {
        jobs_json_append(buf, len, cap, "null", 4);
    }
}

static char* encode_map_to_json(ObjMap* map) {
    char* buf = NULL;
    size_t len = 0, cap = 0;
    jobs_json_value(OBJ_VAL(map), &buf, &len, &cap);
    return buf ? buf : strdup("{}");
}

// ---- Simple JSON decoder for args (returns ObjMap) ----

typedef struct {
    const char* src;
    size_t pos;
    size_t len;
} JParser;

static void jp_skip_ws(JParser* p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->pos++;
        else break;
    }
}

static Value jp_parse_value(JParser* p);

static Value jp_parse_string(JParser* p) {
    if (p->pos >= p->len || p->src[p->pos] != '"') return VAL_NIL;
    p->pos++;

    char* buf = NULL;
    size_t len = 0, cap = 0;

    while (p->pos < p->len && p->src[p->pos] != '"') {
        char c = p->src[p->pos++];
        if (c == '\\' && p->pos < p->len) {
            char esc = p->src[p->pos++];
            switch (esc) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                default: c = esc; break;
            }
        }
        jobs_json_char(&buf, &len, &cap, c);
    }
    if (p->pos < p->len) p->pos++;

    ObjString* str = obj_string_new(buf ? buf : "", (uint32_t)len);
    free(buf);
    return OBJ_VAL(str);
}

static Value jp_parse_number(JParser* p) {
    const char* start = p->src + p->pos;
    bool is_float = false;

    if (p->src[p->pos] == '-') p->pos++;
    while (p->pos < p->len && p->src[p->pos] >= '0' && p->src[p->pos] <= '9') p->pos++;
    if (p->pos < p->len && p->src[p->pos] == '.') {
        is_float = true;
        p->pos++;
        while (p->pos < p->len && p->src[p->pos] >= '0' && p->src[p->pos] <= '9') p->pos++;
    }

    if (is_float) {
        return FLOAT_VAL(strtod(start, NULL));
    }
    return INT_VAL(strtoll(start, NULL, 10));
}

static Value jp_parse_value(JParser* p) {
    jp_skip_ws(p);
    if (p->pos >= p->len) return VAL_NIL;

    char c = p->src[p->pos];

    if (c == '"') return jp_parse_string(p);
    if (c == '-' || (c >= '0' && c <= '9')) return jp_parse_number(p);

    if (c == '{') {
        p->pos++;
        ObjMap* map = obj_map_new();
        gc_push_root(OBJ_VAL(map));

        jp_skip_ws(p);
        if (p->pos < p->len && p->src[p->pos] == '}') {
            p->pos++;
            gc_pop_root();
            return OBJ_VAL(map);
        }

        for (;;) {
            jp_skip_ws(p);
            Value key = jp_parse_string(p);
            if (!IS_STRING(key)) break;

            jp_skip_ws(p);
            if (p->pos < p->len && p->src[p->pos] == ':') p->pos++;

            Value val = jp_parse_value(p);
            obj_map_set(map, AS_STRING(key), val);

            jp_skip_ws(p);
            if (p->pos < p->len && p->src[p->pos] == ',') {
                p->pos++;
            } else {
                break;
            }
        }

        jp_skip_ws(p);
        if (p->pos < p->len && p->src[p->pos] == '}') p->pos++;
        gc_pop_root();
        return OBJ_VAL(map);
    }

    if (strncmp(p->src + p->pos, "true", 4) == 0) {
        p->pos += 4;
        return VAL_TRUE;
    }
    if (strncmp(p->src + p->pos, "false", 5) == 0) {
        p->pos += 5;
        return VAL_FALSE;
    }
    if (strncmp(p->src + p->pos, "null", 4) == 0) {
        p->pos += 4;
        return VAL_NIL;
    }

    return VAL_NIL;
}

// ---- Database helpers ----

static bool ensure_db(void) {
    if (jobs_db) return true;
    jobs_db = turso_connect();
    if (!jobs_db) return false;
    return true;
}

static bool ensure_table(void) {
    if (table_created) return true;
    if (!ensure_db()) return false;

    const char* sql =
        "CREATE TABLE IF NOT EXISTS _vek_jobs ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL,"
        "  args TEXT DEFAULT '{}',"
        "  status TEXT DEFAULT 'pending',"
        "  attempts INTEGER DEFAULT 0,"
        "  run_at INTEGER,"
        "  created_at INTEGER"
        ")";

    int rc = turso_exec_sql(jobs_db, sql);
    if (rc != TURSO_OK) return false;

    table_created = true;
    return true;
}

// ---- Native functions ----

// jobs.enqueue(name, args_map)
static Value native_jobs_enqueue(int argc, Value* args) {
    (void)argc;
    if (!IS_STRING(args[0])) return VAL_NIL;
    if (!ensure_table()) return VAL_NIL;

    ObjString* name = AS_STRING(args[0]);

    char* args_json = NULL;
    if (argc >= 2 && IS_MAP(args[1])) {
        args_json = encode_map_to_json(AS_MAP(args[1]));
    } else {
        args_json = strdup("{}");
    }

    time_t now = time(NULL);

    const char* sql = "INSERT INTO _vek_jobs (name, args, status, attempts, run_at, created_at) "
                      "VALUES (?, ?, 'pending', 0, ?, ?)";

    turso_stmt* stmt = turso_prepare(jobs_db, sql, (int)strlen(sql));
    if (!stmt) {
        free(args_json);
        return VAL_NIL;
    }

    turso_bind_text(stmt, 1, name->data, (int)name->length);
    turso_bind_text(stmt, 2, args_json, (int)strlen(args_json));
    turso_bind_int(stmt, 3, (int64_t)now);
    turso_bind_int(stmt, 4, (int64_t)now);

    int64_t last_id = 0;
    int rc = turso_exec(stmt, &last_id);
    turso_stmt_free(stmt);
    free(args_json);

    if (rc != TURSO_OK) return VAL_NIL;

    return VAL_TRUE;
}

// jobs.process(name, closure) - register a handler
static Value native_jobs_process(int argc, Value* args) {
    (void)argc;
    if (!IS_STRING(args[0])) return VAL_NIL;
    if (argc < 2) return VAL_NIL;
    if (!IS_CLOSURE(args[1]) && !IS_NATIVE(args[1])) return VAL_NIL;
    if (handler_count >= MAX_HANDLERS) return VAL_NIL;

    ObjString* name = AS_STRING(args[0]);
    size_t copy_len = name->length < 127 ? name->length : 127;
    memcpy(handlers[handler_count].name, name->data, copy_len);
    handlers[handler_count].name[copy_len] = '\0';
    handlers[handler_count].closure = args[1];

    vm_pin((ObjHeader*)AS_PTR(args[1]));
    handler_count++;

    return VAL_TRUE;
}

// jobs.run_one() - poll one pending job and run its handler
static Value native_jobs_run_one(int argc, Value* args) {
    (void)argc; (void)args;
    if (!ensure_table()) return VAL_NIL;

    time_t now = time(NULL);

    const char* sql = "SELECT id, name, args FROM _vek_jobs "
                      "WHERE status = 'pending' AND run_at <= ? "
                      "ORDER BY id LIMIT 1";

    turso_stmt* stmt = turso_prepare(jobs_db, sql, (int)strlen(sql));
    if (!stmt) return VAL_NIL;

    turso_bind_int(stmt, 1, (int64_t)now);

    turso_rows* rows = turso_query(stmt);
    turso_stmt_free(stmt);
    if (!rows) return VAL_NIL;

    int count = turso_row_count(rows);
    if (count == 0) {
        turso_rows_free(rows);
        return VAL_FALSE;
    }

    int64_t job_id = turso_row_get_int(rows, 0, 0);
    const char* job_name = turso_row_get_text(rows, 0, 1);
    const char* job_args_json = turso_row_get_text(rows, 0, 2);

    char name_buf[128];
    if (job_name) {
        size_t name_len = strlen(job_name);
        if (name_len > 127) name_len = 127;
        memcpy(name_buf, job_name, name_len);
        name_buf[name_len] = '\0';
    } else {
        name_buf[0] = '\0';
    }

    char* args_copy = strdup(job_args_json ? job_args_json : "{}");
    turso_rows_free(rows);

    // Mark as running
    const char* update_sql = "UPDATE _vek_jobs SET status = 'running', attempts = attempts + 1 WHERE id = ?";
    turso_stmt* update_stmt = turso_prepare(jobs_db, update_sql, (int)strlen(update_sql));
    if (update_stmt) {
        turso_bind_int(update_stmt, 1, job_id);
        turso_exec(update_stmt, NULL);
        turso_stmt_free(update_stmt);
    }

    // Find handler
    Value handler = VAL_NIL;
    for (int i = 0; i < handler_count; i++) {
        if (strcmp(handlers[i].name, name_buf) == 0) {
            handler = handlers[i].closure;
            break;
        }
    }

    if (IS_NIL(handler)) {
        const char* revert_sql = "UPDATE _vek_jobs SET status = 'pending' WHERE id = ?";
        turso_stmt* revert_stmt = turso_prepare(jobs_db, revert_sql, (int)strlen(revert_sql));
        if (revert_stmt) {
            turso_bind_int(revert_stmt, 1, job_id);
            turso_exec(revert_stmt, NULL);
            turso_stmt_free(revert_stmt);
        }
        free(args_copy);
        return VAL_FALSE;
    }

    // Parse args JSON into ObjMap
    JParser parser = { .src = args_copy, .pos = 0, .len = strlen(args_copy) };
    Value args_val = jp_parse_value(&parser);
    free(args_copy);

    // Call the handler
    vm_push(handler);
    vm_push(args_val);
    Value result = vm_call(handler, 1);

    // Mark as completed or failed
    const char* done_sql;
    if (!IS_NIL(result) && result != VAL_FALSE) {
        done_sql = "UPDATE _vek_jobs SET status = 'completed' WHERE id = ?";
    } else {
        done_sql = "UPDATE _vek_jobs SET status = 'failed' WHERE id = ?";
    }

    turso_stmt* done_stmt = turso_prepare(jobs_db, done_sql, (int)strlen(done_sql));
    if (done_stmt) {
        turso_bind_int(done_stmt, 1, job_id);
        turso_exec(done_stmt, NULL);
        turso_stmt_free(done_stmt);
    }

    return VAL_TRUE;
}

// jobs.clear() - delete all jobs (for testing)
static Value native_jobs_clear(int argc, Value* args) {
    (void)argc; (void)args;
    if (!ensure_table()) return VAL_NIL;
    turso_exec_sql(jobs_db, "DELETE FROM _vek_jobs");
    return VAL_TRUE;
}

// jobs.pending() - count of pending jobs
static Value native_jobs_pending(int argc, Value* args) {
    (void)argc; (void)args;
    if (!ensure_table()) return INT_VAL(0);

    const char* sql = "SELECT COUNT(*) FROM _vek_jobs WHERE status = 'pending'";
    turso_stmt* stmt = turso_prepare(jobs_db, sql, (int)strlen(sql));
    if (!stmt) return INT_VAL(0);

    turso_rows* rows = turso_query(stmt);
    turso_stmt_free(stmt);
    if (!rows) return INT_VAL(0);

    int64_t cnt = 0;
    if (turso_row_count(rows) > 0) {
        cnt = turso_row_get_int(rows, 0, 0);
    }
    turso_rows_free(rows);

    return INT_VAL(cnt);
}

void stdlib_jobs_init(ObjMap* pkg) {
    stdlib_register(pkg, "enqueue", native_jobs_enqueue, -1);
    stdlib_register(pkg, "process", native_jobs_process, 2);
    stdlib_register(pkg, "run_one", native_jobs_run_one, 0);
    stdlib_register(pkg, "pending", native_jobs_pending, 0);
    stdlib_register(pkg, "clear", native_jobs_clear, 0);
}
