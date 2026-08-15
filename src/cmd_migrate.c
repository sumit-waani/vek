#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "cli.h"
#include "vek_db.h"

#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

#define MIGRATIONS_TABLE_SQL \
    "CREATE TABLE IF NOT EXISTS _migrations (" \
    "  id INTEGER PRIMARY KEY AUTOINCREMENT," \
    "  filename TEXT NOT NULL UNIQUE," \
    "  applied_at TEXT NOT NULL DEFAULT (datetime('now'))" \
    ")"

static vek_db_conn* open_database(void) {
    vek_db_conn* conn = vek_db_connect();
    if (!conn) {
        fprintf(stderr, "Error: could not open database.\n");
        fprintf(stderr, "Set DATABASE_PATH environment variable or use the default (app.db).\n");
        return NULL;
    }

    int rc = vek_db_exec_sql(conn, MIGRATIONS_TABLE_SQL);
    if (rc != VEK_DB_OK) {
        fprintf(stderr, "Error: could not create migrations table.\n");
        vek_db_disconnect(conn);
        return NULL;
    }

    return conn;
}

static bool is_migration_applied(vek_db_conn* conn, const char* filename) {
    const char* sql = "SELECT COUNT(*) FROM _migrations WHERE filename = ?";
    vek_db_stmt* stmt = vek_db_prepare(conn, sql, (int)strlen(sql));
    if (!stmt) return false;

    vek_db_bind_text(stmt, 1, filename, (int)strlen(filename));
    vek_db_rows* rows = vek_db_query(stmt);
    vek_db_stmt_free(stmt);
    if (!rows) return false;

    bool applied = false;
    if (vek_db_row_count(rows) > 0) {
        applied = vek_db_row_get_int(rows, 0, 0) > 0;
    }
    vek_db_rows_free(rows);
    return applied;
}

static const char* get_migration_timestamp(vek_db_conn* conn, const char* filename, char* buf, size_t buf_size) {
    const char* sql = "SELECT applied_at FROM _migrations WHERE filename = ?";
    vek_db_stmt* stmt = vek_db_prepare(conn, sql, (int)strlen(sql));
    if (!stmt) return NULL;

    vek_db_bind_text(stmt, 1, filename, (int)strlen(filename));
    vek_db_rows* rows = vek_db_query(stmt);
    vek_db_stmt_free(stmt);
    if (!rows) return NULL;

    if (vek_db_row_count(rows) > 0) {
        const char* ts = vek_db_row_get_text(rows, 0, 0);
        if (ts) {
            snprintf(buf, buf_size, "%s", ts);
            vek_db_rows_free(rows);
            return buf;
        }
    }
    vek_db_rows_free(rows);
    return NULL;
}

static char* read_file_contents(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    if (size < 0) { fclose(f); return NULL; }

    char* buf = (char*)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t nread = fread(buf, 1, (size_t)size, f);
    buf[nread] = '\0';
    fclose(f);
    return buf;
}

static int compare_filenames(const void* a, const void* b) {
    const char* fa = *(const char**)a;
    const char* fb = *(const char**)b;
    return strcmp(fa, fb);
}

static bool is_migration_file(const char* name) {
    size_t len = strlen(name);
    if (len < 10) return false;
    for (int i = 0; i < 4; i++) {
        if (name[i] < '0' || name[i] > '9') return false;
    }
    if (name[4] != '_') return false;
    if (strcmp(name + len - 4, ".sql") != 0) return false;
    return true;
}

static char** scan_migrations(int* count) {
    *count = 0;
    DIR* dir = opendir("migrations");
    if (!dir) return NULL;

    int capacity = 32;
    char** files = (char**)malloc(sizeof(char*) * (size_t)capacity);
    if (!files) { closedir(dir); return NULL; }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (!is_migration_file(entry->d_name)) continue;

        if (*count >= capacity) {
            capacity *= 2;
            char** new_files = (char**)realloc(files, sizeof(char*) * (size_t)capacity);
            if (!new_files) {
                for (int i = 0; i < *count; i++) free(files[i]);
                free(files); closedir(dir); *count = 0;
                return NULL;
            }
            files = new_files;
        }

        files[*count] = strdup(entry->d_name);
        (*count)++;
    }
    closedir(dir);

    if (*count > 1) {
        qsort(files, (size_t)*count, sizeof(char*), compare_filenames);
    }
    return files;
}

static void free_file_list(char** files, int count) {
    if (!files) return;
    for (int i = 0; i < count; i++) free(files[i]);
    free(files);
}

static int migrate_apply(void) {
    vek_db_conn* conn = open_database();
    if (!conn) return 1;

    bool color = cli_color_enabled();
    int file_count = 0;
    char** files = scan_migrations(&file_count);

    if (files == NULL || file_count == 0) {
        printf("No migration files found in migrations/ directory.\n");
        free_file_list(files, file_count);
        vek_db_disconnect(conn);
        return 0;
    }

    int applied = 0;
    int skipped = 0;

    for (int i = 0; i < file_count; i++) {
        const char* filename = files[i];

        if (is_migration_applied(conn, filename)) { skipped++; continue; }

        char filepath[4096];
        snprintf(filepath, sizeof(filepath), "migrations/%s", filename);

        char* sql = read_file_contents(filepath);
        if (!sql) {
            fprintf(stderr, "  Error: could not read '%s': %s\n", filepath, strerror(errno));
            free_file_list(files, file_count);
            vek_db_disconnect(conn);
            return 1;
        }

        int rc = vek_db_begin(conn);
        if (rc != VEK_DB_OK) {
            fprintf(stderr, "  Error: could not begin transaction\n");
            free(sql); free_file_list(files, file_count); vek_db_disconnect(conn);
            return 1;
        }

        rc = vek_db_exec_sql(conn, sql);
        if (rc != VEK_DB_OK) {
            fprintf(stderr, "  Error applying '%s'\n", filename);
            vek_db_rollback(conn); free(sql);
            free_file_list(files, file_count); vek_db_disconnect(conn);
            return 1;
        }
        free(sql);

        const char* insert_sql = "INSERT INTO _migrations (filename) VALUES (?)";
        vek_db_stmt* stmt = vek_db_prepare(conn, insert_sql, (int)strlen(insert_sql));
        if (!stmt) {
            fprintf(stderr, "  Error recording migration '%s'\n", filename);
            vek_db_rollback(conn); free_file_list(files, file_count); vek_db_disconnect(conn);
            return 1;
        }

        vek_db_bind_text(stmt, 1, filename, (int)strlen(filename));
        rc = vek_db_exec(stmt, NULL);
        vek_db_stmt_free(stmt);

        if (rc != VEK_DB_OK) {
            fprintf(stderr, "  Error recording migration '%s'\n", filename);
            vek_db_rollback(conn); free_file_list(files, file_count); vek_db_disconnect(conn);
            return 1;
        }

        rc = vek_db_commit(conn);
        if (rc != VEK_DB_OK) {
            fprintf(stderr, "  Error committing migration '%s'\n", filename);
            free_file_list(files, file_count); vek_db_disconnect(conn);
            return 1;
        }

        if (color) {
            printf("  %s\xe2\x9c\x93%s %s\n", CLI_GREEN, CLI_RESET, filename);
        } else {
            printf("  + %s\n", filename);
        }
        applied++;
    }

    if (applied == 0 && skipped > 0) {
        printf("All migrations already applied (%d total).\n", skipped);
    } else if (applied > 0) {
        printf("\n%d migration(s) applied successfully.\n", applied);
    }

    free_file_list(files, file_count);
    vek_db_disconnect(conn);
    return 0;
}

static int migrate_status(void) {
    vek_db_conn* conn = open_database();
    if (!conn) return 1;

    bool color = cli_color_enabled();
    int file_count = 0;
    char** files = scan_migrations(&file_count);

    if (files == NULL || file_count == 0) {
        printf("No migration files found in migrations/ directory.\n");
        free_file_list(files, file_count);
        vek_db_disconnect(conn);
        return 0;
    }

    if (color) {
        printf("%s%-8s  %-20s  %s%s\n", CLI_BOLD, "STATUS", "APPLIED AT", "FILENAME", CLI_RESET);
    } else {
        printf("%-8s  %-20s  %s\n", "STATUS", "APPLIED AT", "FILENAME");
    }
    printf("--------  --------------------  ----------------------------------------\n");

    for (int i = 0; i < file_count; i++) {
        const char* filename = files[i];
        char ts_buf[64];

        if (is_migration_applied(conn, filename)) {
            const char* ts = get_migration_timestamp(conn, filename, ts_buf, sizeof(ts_buf));
            printf("%-8s  %-20s  %s\n", "applied", ts ? ts : "unknown", filename);
        } else {
            printf("%-8s  %-20s  %s\n", "pending", "-", filename);
        }
    }

    free_file_list(files, file_count);
    vek_db_disconnect(conn);
    return 0;
}

static int migrate_new(int argc, char** argv) {
    const char* name = NULL;
    bool found_new = false;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) == 0) continue;
        if (strcmp(argv[i], "migrate") == 0) continue;
        if (!found_new && strcmp(argv[i], "new") == 0) { found_new = true; continue; }
        if (found_new) { name = argv[i]; break; }
    }

    if (!name || strlen(name) == 0) {
        fprintf(stderr, "Error: 'migrate new' requires a migration name.\n");
        fprintf(stderr, "Usage: vek migrate new <name>\n");
        return 64;
    }

    bool color = cli_color_enabled();

    if (mkdir("migrations", 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "Error: could not create migrations/ directory: %s\n", strerror(errno));
        return 1;
    }

    int file_count = 0;
    char** files = scan_migrations(&file_count);
    int next_num = file_count + 1;
    free_file_list(files, file_count);

    char filename[256];
    snprintf(filename, sizeof(filename), "%04d_%s.sql", next_num, name);

    char filepath[4096];
    snprintf(filepath, sizeof(filepath), "migrations/%s", filename);

    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    FILE* f = fopen(filepath, "w");
    if (!f) {
        fprintf(stderr, "Error: could not create '%s': %s\n", filepath, strerror(errno));
        return 1;
    }

    fprintf(f, "-- Migration: %s\n", name);
    fprintf(f, "-- Created: %s\n", timestamp);
    fprintf(f, "\n");
    fclose(f);

    if (color) {
        printf("  %sCreated:%s %s\n", CLI_GREEN, CLI_RESET, filepath);
    } else {
        printf("  Created: %s\n", filepath);
    }

    return 0;
}

static void print_help(void) {
    printf("Usage: vek migrate [command]\n\n");
    printf("Run and manage database migrations.\n\n");
    printf("Commands:\n");
    printf("  (none)          Apply all pending migrations\n");
    printf("  status          Show migration status (applied/pending)\n");
    printf("  new <name>      Create a new migration file\n\n");
    printf("Environment:\n");
    printf("  DATABASE_PATH   Path to SQLite database (default: app.db)\n\n");
    printf("Examples:\n");
    printf("  vek migrate                     Apply pending migrations\n");
    printf("  vek migrate status              Show which migrations are applied\n");
    printf("  vek migrate new create_users    Create a new migration file\n");
}

int cmd_migrate_run(int argc, char** argv) {
    if (cli_has_flag(argc, argv, "--help") || cli_has_flag(argc, argv, "-h")) {
        print_help();
        return 0;
    }

    const char* subcmd = NULL;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) == 0) continue;
        if (strcmp(argv[i], "migrate") == 0) continue;
        subcmd = argv[i];
        break;
    }

    if (subcmd == NULL) return migrate_apply();
    if (strcmp(subcmd, "status") == 0) return migrate_status();
    if (strcmp(subcmd, "new") == 0) return migrate_new(argc, argv);

    fprintf(stderr, "Error: unknown migrate subcommand '%s'.\n", subcmd);
    fprintf(stderr, "Run 'vek migrate --help' for usage.\n");
    return 64;
}
