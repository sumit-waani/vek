/*
 * vekd_db.h - SQLite state management for vekd.
 *
 * Handles database initialization, schema migration, and CRUD operations
 * for the vekd data model (apps, releases, env_vars, events, secrets, users).
 */
#ifndef VEKD_DB_H
#define VEKD_DB_H

#include "../sqlite3.h"
#include <stdbool.h>
#include <stdint.h>

/* Database handle */
typedef struct {
    sqlite3 *db;
    const char *path;
} VekdDB;

/* Open (or create) the database at the given path and apply schema */
int vekd_db_open(VekdDB *vdb, const char *path);

/* Close the database */
void vekd_db_close(VekdDB *vdb);

/* Initialize schema (called automatically by vekd_db_open) */
int vekd_db_init_schema(VekdDB *vdb);

/* --- App operations --- */
typedef struct {
    int64_t id;
    char name[256];
    char repo_url[1024];
    char branch[128];
    char domain[256];
    int port;
    int cpu_weight;
    int64_t memory_max;
    char state[32];
    char current_release[256];
    int64_t created_at;
    int64_t updated_at;
} VekdApp;

int vekd_db_app_create(VekdDB *vdb, const char *name, const char *repo_url,
                       const char *branch, int port);
int vekd_db_app_get_by_name(VekdDB *vdb, const char *name, VekdApp *app);
int vekd_db_app_update_state(VekdDB *vdb, int64_t app_id, const char *state);
int vekd_db_app_delete(VekdDB *vdb, int64_t app_id);
int vekd_db_app_count(VekdDB *vdb);

/* --- Release operations --- */
typedef struct {
    int64_t id;
    int64_t app_id;
    char ref[128];
    char artifact_path[512];
    int64_t created_at;
} VekdRelease;

int vekd_db_release_create(VekdDB *vdb, int64_t app_id, const char *ref,
                           const char *artifact_path);

/* --- Event operations --- */
int vekd_db_event_log(VekdDB *vdb, int64_t app_id, const char *kind,
                      const char *message);

/* --- User operations --- */
typedef struct {
    int64_t id;
    char email[256];
    char password_hash[128];
    int is_admin;
    int64_t created_at;
} VekdUser;

int vekd_db_user_create(VekdDB *vdb, const char *email,
                        const char *password_hash, int is_admin);
int vekd_db_user_get_by_email(VekdDB *vdb, const char *email, VekdUser *user);
int vekd_db_user_count(VekdDB *vdb);

/* Atomically create user only if no users exist (first-admin creation) */
int vekd_db_user_create_if_no_users(VekdDB *vdb, const char *email,
                                    const char *password_hash, int is_admin);

/* Get the next available port for an app (MAX(port)+1 from DB) */
int vekd_db_get_next_port(VekdDB *vdb);

#endif /* VEKD_DB_H */
