/*
 * vekd_db.c - SQLite state management implementation.
 */
#include "vekd_db.h"
#include "vekd_config.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS apps ("
    "  id INTEGER PRIMARY KEY,"
    "  name TEXT UNIQUE NOT NULL,"
    "  repo_url TEXT NOT NULL,"
    "  branch TEXT NOT NULL DEFAULT 'main',"
    "  pat_secret_ref TEXT,"
    "  domain TEXT UNIQUE,"
    "  port INTEGER UNIQUE,"
    "  cpu_weight INTEGER DEFAULT 100,"
    "  memory_max INTEGER DEFAULT 536870912,"
    "  state TEXT NOT NULL DEFAULT 'pending',"
    "  current_release TEXT,"
    "  created_at INTEGER,"
    "  updated_at INTEGER"
    ");"
    "CREATE TABLE IF NOT EXISTS releases ("
    "  id INTEGER PRIMARY KEY,"
    "  app_id INTEGER REFERENCES apps(id),"
    "  ref TEXT NOT NULL,"
    "  artifact_path TEXT,"
    "  created_at INTEGER,"
    "  deploy_log TEXT"
    ");"
    "CREATE TABLE IF NOT EXISTS env_vars ("
    "  app_id INTEGER REFERENCES apps(id),"
    "  key TEXT,"
    "  value_encrypted BLOB,"
    "  PRIMARY KEY (app_id, key)"
    ");"
    "CREATE TABLE IF NOT EXISTS events ("
    "  id INTEGER PRIMARY KEY,"
    "  app_id INTEGER REFERENCES apps(id),"
    "  kind TEXT,"
    "  message TEXT,"
    "  created_at INTEGER"
    ");"
    "CREATE TABLE IF NOT EXISTS secrets ("
    "  id INTEGER PRIMARY KEY,"
    "  name TEXT UNIQUE,"
    "  value_encrypted BLOB"
    ");"
    "CREATE TABLE IF NOT EXISTS users ("
    "  id INTEGER PRIMARY KEY,"
    "  email TEXT UNIQUE,"
    "  password_hash TEXT,"
    "  totp_secret TEXT,"
    "  is_admin INTEGER,"
    "  created_at INTEGER"
    ");";

int vekd_db_open(VekdDB *vdb, const char *path) {
    vdb->path = path;
    int rc = sqlite3_open(path, &vdb->db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "vekd: failed to open database: %s\n",
                sqlite3_errmsg(vdb->db));
        return -1;
    }

    /* Enable WAL mode for better concurrency */
    sqlite3_exec(vdb->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(vdb->db, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);

    return vekd_db_init_schema(vdb);
}

void vekd_db_close(VekdDB *vdb) {
    if (vdb->db) {
        sqlite3_close(vdb->db);
        vdb->db = NULL;
    }
}

int vekd_db_init_schema(VekdDB *vdb) {
    char *err = NULL;
    int rc = sqlite3_exec(vdb->db, SCHEMA_SQL, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "vekd: schema init failed: %s\n", err);
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

/* --- App operations --- */

int vekd_db_app_create(VekdDB *vdb, const char *name, const char *repo_url,
                       const char *branch, int port) {
    const char *sql =
        "INSERT INTO apps (name, repo_url, branch, port, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?)";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(vdb->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    int64_t now = (int64_t)time(NULL);
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, repo_url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, branch, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, port);
    sqlite3_bind_int64(stmt, 5, now);
    sqlite3_bind_int64(stmt, 6, now);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int vekd_db_app_get_by_name(VekdDB *vdb, const char *name, VekdApp *app) {
    const char *sql = "SELECT id, name, repo_url, branch, domain, port, "
                      "cpu_weight, memory_max, state, current_release, "
                      "created_at, updated_at FROM apps WHERE name = ?";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(vdb->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return -1;
    }

    memset(app, 0, sizeof(VekdApp));
    app->id = sqlite3_column_int64(stmt, 0);

    const char *col_text;
    col_text = (const char *)sqlite3_column_text(stmt, 1);
    if (col_text) strncpy(app->name, col_text, sizeof(app->name) - 1);

    col_text = (const char *)sqlite3_column_text(stmt, 2);
    if (col_text) strncpy(app->repo_url, col_text, sizeof(app->repo_url) - 1);

    col_text = (const char *)sqlite3_column_text(stmt, 3);
    if (col_text) strncpy(app->branch, col_text, sizeof(app->branch) - 1);

    col_text = (const char *)sqlite3_column_text(stmt, 4);
    if (col_text) strncpy(app->domain, col_text, sizeof(app->domain) - 1);

    app->port = sqlite3_column_int(stmt, 5);
    app->cpu_weight = sqlite3_column_int(stmt, 6);
    app->memory_max = sqlite3_column_int64(stmt, 7);

    col_text = (const char *)sqlite3_column_text(stmt, 8);
    if (col_text) strncpy(app->state, col_text, sizeof(app->state) - 1);

    col_text = (const char *)sqlite3_column_text(stmt, 9);
    if (col_text) strncpy(app->current_release, col_text, sizeof(app->current_release) - 1);

    app->created_at = sqlite3_column_int64(stmt, 10);
    app->updated_at = sqlite3_column_int64(stmt, 11);

    sqlite3_finalize(stmt);
    return 0;
}

int vekd_db_app_update_state(VekdDB *vdb, int64_t app_id, const char *state) {
    const char *sql = "UPDATE apps SET state = ?, updated_at = ? WHERE id = ?";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(vdb->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    int64_t now = (int64_t)time(NULL);
    sqlite3_bind_text(stmt, 1, state, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, now);
    sqlite3_bind_int64(stmt, 3, app_id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int vekd_db_app_delete(VekdDB *vdb, int64_t app_id) {
    /* Delete related rows first (cascading delete) */
    const char *del_env = "DELETE FROM env_vars WHERE app_id = ?";
    const char *del_events = "DELETE FROM events WHERE app_id = ?";
    const char *del_releases = "DELETE FROM releases WHERE app_id = ?";
    const char *del_app = "DELETE FROM apps WHERE id = ?";

    sqlite3_stmt *stmt;
    int rc;

    /* Delete env_vars */
    rc = sqlite3_prepare_v2(vdb->db, del_env, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, app_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    /* Delete events */
    rc = sqlite3_prepare_v2(vdb->db, del_events, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, app_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    /* Delete releases */
    rc = sqlite3_prepare_v2(vdb->db, del_releases, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, app_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    /* Delete the app itself */
    rc = sqlite3_prepare_v2(vdb->db, del_app, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_int64(stmt, 1, app_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int vekd_db_app_count(VekdDB *vdb) {
    const char *sql = "SELECT COUNT(*) FROM apps";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(vdb->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    rc = sqlite3_step(stmt);
    int count = (rc == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : -1;
    sqlite3_finalize(stmt);
    return count;
}

/* --- Release operations --- */

int vekd_db_release_create(VekdDB *vdb, int64_t app_id, const char *ref,
                           const char *artifact_path) {
    const char *sql =
        "INSERT INTO releases (app_id, ref, artifact_path, created_at) "
        "VALUES (?, ?, ?, ?)";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(vdb->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    int64_t now = (int64_t)time(NULL);
    sqlite3_bind_int64(stmt, 1, app_id);
    sqlite3_bind_text(stmt, 2, ref, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, artifact_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, now);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* --- Event operations --- */

int vekd_db_event_log(VekdDB *vdb, int64_t app_id, const char *kind,
                      const char *message) {
    const char *sql =
        "INSERT INTO events (app_id, kind, message, created_at) "
        "VALUES (?, ?, ?, ?)";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(vdb->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    int64_t now = (int64_t)time(NULL);
    sqlite3_bind_int64(stmt, 1, app_id);
    sqlite3_bind_text(stmt, 2, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, message, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, now);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* --- User operations --- */

int vekd_db_user_create(VekdDB *vdb, const char *email,
                        const char *password_hash, int is_admin) {
    const char *sql =
        "INSERT INTO users (email, password_hash, is_admin, created_at) "
        "VALUES (?, ?, ?, ?)";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(vdb->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    int64_t now = (int64_t)time(NULL);
    sqlite3_bind_text(stmt, 1, email, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, password_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, is_admin);
    sqlite3_bind_int64(stmt, 4, now);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int vekd_db_user_get_by_email(VekdDB *vdb, const char *email, VekdUser *user) {
    const char *sql = "SELECT id, email, password_hash, is_admin, created_at "
                      "FROM users WHERE email = ?";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(vdb->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt, 1, email, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return -1;
    }

    memset(user, 0, sizeof(VekdUser));
    user->id = sqlite3_column_int64(stmt, 0);

    const char *col_text;
    col_text = (const char *)sqlite3_column_text(stmt, 1);
    if (col_text) strncpy(user->email, col_text, sizeof(user->email) - 1);

    col_text = (const char *)sqlite3_column_text(stmt, 2);
    if (col_text) strncpy(user->password_hash, col_text, sizeof(user->password_hash) - 1);

    user->is_admin = sqlite3_column_int(stmt, 3);
    user->created_at = sqlite3_column_int64(stmt, 4);

    sqlite3_finalize(stmt);
    return 0;
}

int vekd_db_user_count(VekdDB *vdb) {
    const char *sql = "SELECT COUNT(*) FROM users";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(vdb->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    rc = sqlite3_step(stmt);
    int count = (rc == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : -1;
    sqlite3_finalize(stmt);
    return count;
}

/* --- Port assignment --- */

int vekd_db_get_next_port(VekdDB *vdb) {
    const char *sql = "SELECT COALESCE(MAX(port), 9999) + 1 FROM apps";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(vdb->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return VEKD_APP_PORT_MIN;

    rc = sqlite3_step(stmt);
    int port = VEKD_APP_PORT_MIN;
    if (rc == SQLITE_ROW) {
        port = sqlite3_column_int(stmt, 0);
        if (port < VEKD_APP_PORT_MIN) port = VEKD_APP_PORT_MIN;
    }
    sqlite3_finalize(stmt);
    return port;
}

/* --- Atomic first-admin creation --- */

int vekd_db_user_create_if_no_users(VekdDB *vdb, const char *email,
                                    const char *password_hash, int is_admin) {
    /* Atomic INSERT that only succeeds if users table is empty */
    const char *sql =
        "INSERT INTO users (email, password_hash, is_admin, created_at) "
        "SELECT ?, ?, ?, ? WHERE (SELECT COUNT(*) FROM users) = 0";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(vdb->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    int64_t now = (int64_t)time(NULL);
    sqlite3_bind_text(stmt, 1, email, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, password_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, is_admin);
    sqlite3_bind_int64(stmt, 4, now);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}
