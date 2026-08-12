#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "cli.h"
#include "sqlite3.h"

#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

// Default database path
#define DEFAULT_DB_PATH "data/app.db"

// Migration tracking table schema
#define MIGRATIONS_TABLE_SQL \
    "CREATE TABLE IF NOT EXISTS _migrations (" \
    "  id INTEGER PRIMARY KEY AUTOINCREMENT," \
    "  filename TEXT NOT NULL UNIQUE," \
    "  applied_at TEXT NOT NULL DEFAULT (datetime('now'))" \
    ")"

// ---- Internal helpers ----

// Parse --db=<path> or --db <path> option
static const char* parse_db_path(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--db=", 5) == 0) {
            return argv[i] + 5;
        }
        if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
            return argv[i + 1];
        }
    }
    return DEFAULT_DB_PATH;
}

// Create parent directories for a path (like mkdir -p for parent)
static bool ensure_parent_dir(const char* path) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);

    // Find last slash
    char* last_slash = strrchr(tmp, '/');
    if (last_slash == NULL) return true; // no directory component

    *last_slash = '\0';

    // Try to create the directory (and parents)
    if (mkdir(tmp, 0755) == 0) return true;
    if (errno == EEXIST) return true;

    // Need to create parent directories
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return false;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

// Open database, creating parent directories if needed
static sqlite3* open_database(const char* path) {
    if (!ensure_parent_dir(path)) {
        fprintf(stderr, "Error: could not create directory for database '%s': %s\n",
                path, strerror(errno));
        return NULL;
    }

    sqlite3* db = NULL;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Error: could not open database '%s': %s\n",
                path, db ? sqlite3_errmsg(db) : "unknown error");
        if (db) sqlite3_close(db);
        return NULL;
    }

    // Create migrations tracking table
    char* err_msg = NULL;
    rc = sqlite3_exec(db, MIGRATIONS_TABLE_SQL, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Error: could not create migrations table: %s\n",
                err_msg ? err_msg : "unknown error");
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return NULL;
    }

    return db;
}

// Check if a migration has been applied
static bool is_migration_applied(sqlite3* db, const char* filename) {
    const char* sql = "SELECT COUNT(*) FROM _migrations WHERE filename = ?";
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, filename, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    bool applied = false;
    if (rc == SQLITE_ROW) {
        applied = sqlite3_column_int(stmt, 0) > 0;
    }
    sqlite3_finalize(stmt);
    return applied;
}

// Get the applied_at timestamp for a migration
static const char* get_migration_timestamp(sqlite3* db, const char* filename, char* buf, size_t buf_size) {
    const char* sql = "SELECT applied_at FROM _migrations WHERE filename = ?";
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;

    sqlite3_bind_text(stmt, 1, filename, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const char* ts = (const char*)sqlite3_column_text(stmt, 0);
        if (ts) {
            snprintf(buf, buf_size, "%s", ts);
            sqlite3_finalize(stmt);
            return buf;
        }
    }
    sqlite3_finalize(stmt);
    return NULL;
}

// Read a file into a malloc'd buffer
static char* read_file_contents(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    if (size < 0) {
        fclose(f);
        return NULL;
    }

    char* buf = (char*)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t read = fread(buf, 1, (size_t)size, f);
    buf[read] = '\0';
    fclose(f);
    return buf;
}

// Comparison function for sorting migration filenames
static int compare_filenames(const void* a, const void* b) {
    const char* fa = *(const char**)a;
    const char* fb = *(const char**)b;
    return strcmp(fa, fb);
}

// Check if filename matches migration pattern NNNN_name.sql
static bool is_migration_file(const char* name) {
    size_t len = strlen(name);
    if (len < 10) return false;

    // Check digits at start
    for (int i = 0; i < 4; i++) {
        if (name[i] < '0' || name[i] > '9') return false;
    }

    // Check underscore after digits
    if (name[4] != '_') return false;

    // Check .sql extension
    if (strcmp(name + len - 4, ".sql") != 0) return false;

    return true;
}

// Scan migrations directory for .sql files, returns sorted array
// Sets *count to number of files found
// Caller must free the returned array and each string in it
static char** scan_migrations(int* count) {
    *count = 0;
    DIR* dir = opendir("migrations");
    if (!dir) return NULL;

    int capacity = 32;
    char** files = (char**)malloc(sizeof(char*) * (size_t)capacity);
    if (!files) {
        closedir(dir);
        return NULL;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (!is_migration_file(entry->d_name)) continue;

        if (*count >= capacity) {
            capacity *= 2;
            char** new_files = (char**)realloc(files, sizeof(char*) * (size_t)capacity);
            if (!new_files) {
                for (int i = 0; i < *count; i++) free(files[i]);
                free(files);
                closedir(dir);
                *count = 0;
                return NULL;
            }
            files = new_files;
        }

        files[*count] = strdup(entry->d_name);
        (*count)++;
    }
    closedir(dir);

    // Sort by filename (numeric prefix ensures correct order)
    if (*count > 1) {
        qsort(files, (size_t)*count, sizeof(char*), compare_filenames);
    }

    return files;
}

static void free_file_list(char** files, int count) {
    if (!files) return;
    for (int i = 0; i < count; i++) {
        free(files[i]);
    }
    free(files);
}

// ---- Subcommand implementations ----

// Apply pending migrations
static int migrate_apply(int argc, char** argv) {
    const char* db_path = parse_db_path(argc, argv);
    sqlite3* db = open_database(db_path);
    if (!db) return 1;

    bool color = cli_color_enabled();

    int file_count = 0;
    char** files = scan_migrations(&file_count);

    if (files == NULL || file_count == 0) {
        if (color) {
            printf("%sNo migration files found in migrations/ directory.%s\n", CLI_DIM, CLI_RESET);
        } else {
            printf("No migration files found in migrations/ directory.\n");
        }
        free_file_list(files, file_count);
        sqlite3_close(db);
        return 0;
    }

    int applied = 0;
    int skipped = 0;

    for (int i = 0; i < file_count; i++) {
        const char* filename = files[i];

        // Check if already applied
        if (is_migration_applied(db, filename)) {
            skipped++;
            continue;
        }

        // Read the migration file
        char filepath[4096];
        snprintf(filepath, sizeof(filepath), "migrations/%s", filename);

        char* sql = read_file_contents(filepath);
        if (!sql) {
            if (color) {
                fprintf(stderr, "%s  Error: could not read '%s': %s%s\n",
                        CLI_RED, filepath, strerror(errno), CLI_RESET);
            } else {
                fprintf(stderr, "  Error: could not read '%s': %s\n",
                        filepath, strerror(errno));
            }
            free_file_list(files, file_count);
            sqlite3_close(db);
            return 1;
        }

        // Begin transaction
        char* err_msg = NULL;
        int rc = sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, &err_msg);
        if (rc != SQLITE_OK) {
            if (color) {
                fprintf(stderr, "%s  Error: could not begin transaction: %s%s\n",
                        CLI_RED, err_msg ? err_msg : "unknown", CLI_RESET);
            } else {
                fprintf(stderr, "  Error: could not begin transaction: %s\n",
                        err_msg ? err_msg : "unknown");
            }
            sqlite3_free(err_msg);
            free(sql);
            free_file_list(files, file_count);
            sqlite3_close(db);
            return 1;
        }

        // Execute migration SQL
        rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
        if (rc != SQLITE_OK) {
            if (color) {
                fprintf(stderr, "%s  Error applying '%s': %s%s\n",
                        CLI_RED, filename, err_msg ? err_msg : "unknown", CLI_RESET);
            } else {
                fprintf(stderr, "  Error applying '%s': %s\n",
                        filename, err_msg ? err_msg : "unknown");
            }
            sqlite3_free(err_msg);
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            free(sql);
            free_file_list(files, file_count);
            sqlite3_close(db);
            return 1;
        }
        free(sql);

        // Record the migration
        const char* insert_sql = "INSERT INTO _migrations (filename) VALUES (?)";
        sqlite3_stmt* stmt = NULL;
        rc = sqlite3_prepare_v2(db, insert_sql, -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            if (color) {
                fprintf(stderr, "%s  Error recording migration '%s': %s%s\n",
                        CLI_RED, filename, sqlite3_errmsg(db), CLI_RESET);
            } else {
                fprintf(stderr, "  Error recording migration '%s': %s\n",
                        filename, sqlite3_errmsg(db));
            }
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            free_file_list(files, file_count);
            sqlite3_close(db);
            return 1;
        }

        sqlite3_bind_text(stmt, 1, filename, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            if (color) {
                fprintf(stderr, "%s  Error recording migration '%s': %s%s\n",
                        CLI_RED, filename, sqlite3_errmsg(db), CLI_RESET);
            } else {
                fprintf(stderr, "  Error recording migration '%s': %s\n",
                        filename, sqlite3_errmsg(db));
            }
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            free_file_list(files, file_count);
            sqlite3_close(db);
            return 1;
        }

        // Commit
        rc = sqlite3_exec(db, "COMMIT", NULL, NULL, &err_msg);
        if (rc != SQLITE_OK) {
            if (color) {
                fprintf(stderr, "%s  Error committing migration '%s': %s%s\n",
                        CLI_RED, filename, err_msg ? err_msg : "unknown", CLI_RESET);
            } else {
                fprintf(stderr, "  Error committing migration '%s': %s\n",
                        filename, err_msg ? err_msg : "unknown");
            }
            sqlite3_free(err_msg);
            free_file_list(files, file_count);
            sqlite3_close(db);
            return 1;
        }

        // Success - print green checkmark
        if (color) {
            printf("  %s\xe2\x9c\x93%s %s\n", CLI_GREEN, CLI_RESET, filename);
        } else {
            printf("  + %s\n", filename);
        }
        applied++;
    }

    // Summary
    if (applied == 0 && skipped > 0) {
        if (color) {
            printf("%sAll migrations already applied (%d total).%s\n", CLI_DIM, skipped, CLI_RESET);
        } else {
            printf("All migrations already applied (%d total).\n", skipped);
        }
    } else if (applied > 0) {
        if (color) {
            printf("\n%s%d migration(s) applied successfully.%s\n", CLI_GREEN, applied, CLI_RESET);
        } else {
            printf("\n%d migration(s) applied successfully.\n", applied);
        }
    }

    free_file_list(files, file_count);
    sqlite3_close(db);
    return 0;
}

// Show migration status
static int migrate_status(int argc, char** argv) {
    const char* db_path = parse_db_path(argc, argv);
    sqlite3* db = open_database(db_path);
    if (!db) return 1;

    bool color = cli_color_enabled();

    int file_count = 0;
    char** files = scan_migrations(&file_count);

    if (files == NULL || file_count == 0) {
        if (color) {
            printf("%sNo migration files found in migrations/ directory.%s\n", CLI_DIM, CLI_RESET);
        } else {
            printf("No migration files found in migrations/ directory.\n");
        }
        free_file_list(files, file_count);
        sqlite3_close(db);
        return 0;
    }

    // Print header
    if (color) {
        printf("%s%-8s  %-20s  %s%s\n", CLI_BOLD, "STATUS", "APPLIED AT", "FILENAME", CLI_RESET);
    } else {
        printf("%-8s  %-20s  %s\n", "STATUS", "APPLIED AT", "FILENAME");
    }
    printf("--------  --------------------  ");
    printf("----------------------------------------\n");

    for (int i = 0; i < file_count; i++) {
        const char* filename = files[i];
        char ts_buf[64];

        if (is_migration_applied(db, filename)) {
            const char* ts = get_migration_timestamp(db, filename, ts_buf, sizeof(ts_buf));
            if (color) {
                printf("%s%-8s%s  %-20s  %s\n", CLI_GREEN, "applied", CLI_RESET,
                       ts ? ts : "unknown", filename);
            } else {
                printf("%-8s  %-20s  %s\n", "applied", ts ? ts : "unknown", filename);
            }
        } else {
            if (color) {
                printf("%s%-8s%s  %-20s  %s\n", CLI_YELLOW, "pending", CLI_RESET,
                       "-", filename);
            } else {
                printf("%-8s  %-20s  %s\n", "pending", "-", filename);
            }
        }
    }

    free_file_list(files, file_count);
    sqlite3_close(db);
    return 0;
}

// Create a new migration file
static int migrate_new(int argc, char** argv) {
    // Find the migration name (first non-flag arg after "new")
    const char* name = NULL;
    bool found_new = false;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) == 0) {
            if (strcmp(argv[i], "--db") == 0) { i++; continue; }
            if (strncmp(argv[i], "--db=", 5) == 0) continue;
            continue;
        }
        if (strcmp(argv[i], "migrate") == 0) continue;
        if (!found_new && strcmp(argv[i], "new") == 0) {
            found_new = true;
            continue;
        }
        if (found_new) {
            name = argv[i];
            break;
        }
    }

    if (!name || strlen(name) == 0) {
        fprintf(stderr, "Error: 'migrate new' requires a migration name.\n");
        fprintf(stderr, "Usage: vek migrate new <name>\n");
        return 64; // EX_USAGE
    }

    bool color = cli_color_enabled();

    // Create migrations/ directory if needed
    if (mkdir("migrations", 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "Error: could not create migrations/ directory: %s\n", strerror(errno));
        return 1;
    }

    // Determine next sequence number
    int file_count = 0;
    char** files = scan_migrations(&file_count);
    int next_num = file_count + 1;
    free_file_list(files, file_count);

    // Generate filename
    char filename[256];
    snprintf(filename, sizeof(filename), "%04d_%s.sql", next_num, name);

    // Generate file path
    char filepath[4096];
    snprintf(filepath, sizeof(filepath), "migrations/%s", filename);

    // Get current timestamp
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    // Write the template
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

// ---- Help text ----

static void print_help(void) {
    bool color = cli_color_enabled();

    if (color) {
        printf("%sUsage:%s vek migrate [command] [options]\n\n", CLI_BOLD, CLI_RESET);
    } else {
        printf("Usage: vek migrate [command] [options]\n\n");
    }

    printf("Run and manage database migrations.\n\n");

    if (color) {
        printf("%sCommands:%s\n", CLI_BOLD, CLI_RESET);
    } else {
        printf("Commands:\n");
    }

    printf("  (none)          Apply all pending migrations\n");
    printf("  status          Show migration status (applied/pending)\n");
    printf("  new <name>      Create a new migration file\n");
    printf("\n");

    if (color) {
        printf("%sOptions:%s\n", CLI_BOLD, CLI_RESET);
    } else {
        printf("Options:\n");
    }

    printf("  --db=<path>     Database path (default: %s)\n", DEFAULT_DB_PATH);
    printf("  --help          Show this help message\n");
    printf("\n");

    if (color) {
        printf("%sExamples:%s\n", CLI_BOLD, CLI_RESET);
    } else {
        printf("Examples:\n");
    }

    printf("  vek migrate                     Apply pending migrations\n");
    printf("  vek migrate status              Show which migrations are applied\n");
    printf("  vek migrate new create_users    Create a new migration file\n");
    printf("  vek migrate --db=prod.db        Use a custom database path\n");
}

// ---- Public entry point ----

int cmd_migrate_run(int argc, char** argv) {
    // Check for --help
    if (cli_has_flag(argc, argv, "--help") || cli_has_flag(argc, argv, "-h")) {
        print_help();
        return 0;
    }

    // Find subcommand (first non-flag arg after "migrate")
    const char* subcmd = NULL;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) == 0) {
            // Skip flags with values
            if (strcmp(argv[i], "--db") == 0) { i++; continue; }
            continue;
        }
        if (strcmp(argv[i], "migrate") == 0) continue;
        subcmd = argv[i];
        break;
    }

    if (subcmd == NULL) {
        // No subcommand: apply pending migrations
        return migrate_apply(argc, argv);
    }

    if (strcmp(subcmd, "status") == 0) {
        return migrate_status(argc, argv);
    }

    if (strcmp(subcmd, "new") == 0) {
        return migrate_new(argc, argv);
    }

    fprintf(stderr, "Error: unknown migrate subcommand '%s'.\n", subcmd);
    fprintf(stderr, "Run 'vek migrate --help' for usage.\n");
    return 64;
}
