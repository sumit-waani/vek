/*
 * vekd_main.c - Entry point for the vekd daemon binary.
 *
 * vekd is a long-running system service that provides a web dashboard
 * on port 8080 for managing production deployments. On startup it:
 *   1. Checks if /var/lib/vek/ exists (first-run detection)
 *   2. Initializes the SQLite database
 *   3. Loads or generates the master key
 *   4. Starts the HTTP server on port 8080
 */
#define _GNU_SOURCE
#include "vekd_config.h"
#include "vekd_db.h"
#include "vekd_crypto.h"
#include "vekd_supervisor.h"
#include "vekd_web.h"
#include "../http_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

/* Global state */
static VekdDB g_db;
static VekdSupervisor g_supervisor;
static uint8_t g_master_key[VEKD_KEY_SIZE];
static HttpServer g_server;

/* Allow overriding paths for testing */
static const char *g_data_dir = VEKD_DATA_DIR;
static const char *g_db_path = VEKD_DB_PATH;
static const char *g_key_path = VEKD_MASTER_KEY_PATH;
static int g_port = VEKD_DEFAULT_PORT;

static void signal_handler(int sig) {
    (void)sig;
    http_server_stop(&g_server);
}

static void print_version(void) {
    printf("vekd %s\n", VEKD_VERSION);
}

static void print_usage(void) {
    printf("Usage: vekd [OPTIONS]\n\n");
    printf("vekd is a web-based dashboard for managing production deployments.\n");
    printf("Visit http://localhost:%d after starting to access the dashboard.\n\n", VEKD_DEFAULT_PORT);
    printf("Options:\n");
    printf("  --port PORT       Listen port (default: %d)\n", VEKD_DEFAULT_PORT);
    printf("  --data-dir DIR    Data directory (default: %s)\n", VEKD_DATA_DIR);
    printf("  --version         Show version\n");
    printf("  --help            Show this help\n");
}

static int ensure_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        fprintf(stderr, "vekd: %s exists but is not a directory\n", path);
        return -1;
    }
    if (mkdir(path, 0755) < 0) {
        fprintf(stderr, "vekd: cannot create %s: %s\n", path, strerror(errno));
        return -1;
    }
    printf("vekd: created data directory %s\n", path);
    return 0;
}

static int parse_args(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            print_version();
            exit(0);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage();
            exit(0);
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_port = atoi(argv[++i]);
            if (g_port <= 0 || g_port > 65535) {
                fprintf(stderr, "vekd: invalid port number\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
            g_data_dir = argv[++i];
            /* Rebuild derived paths */
            static char db_buf[512];
            static char key_buf[512];
            snprintf(db_buf, sizeof(db_buf), "%s/vekd.db", g_data_dir);
            snprintf(key_buf, sizeof(key_buf), "%s/master.key", g_data_dir);
            g_db_path = db_buf;
            g_key_path = key_buf;
        } else {
            fprintf(stderr, "vekd: unknown option: %s\n", argv[i]);
            print_usage();
            return -1;
        }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (parse_args(argc, argv) < 0) return 1;

    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Step 1: Ensure data directory exists */
    if (ensure_directory(g_data_dir) < 0) return 1;

    /* Step 2: Initialize database */
    printf("vekd: opening database at %s\n", g_db_path);
    if (vekd_db_open(&g_db, g_db_path) < 0) {
        fprintf(stderr, "vekd: failed to initialize database\n");
        return 1;
    }
    printf("vekd: database initialized\n");

    /* Step 3: Load or generate master key */
    printf("vekd: loading master key from %s\n", g_key_path);
    if (vekd_crypto_init_key(g_key_path, g_master_key) < 0) {
        fprintf(stderr, "vekd: failed to initialize master key\n");
        vekd_db_close(&g_db);
        return 1;
    }
    printf("vekd: master key ready\n");

    /* Step 4: Initialize supervisor */
    vekd_supervisor_init(&g_supervisor);
    if (g_supervisor.use_systemd) {
        printf("vekd: systemd integration available\n");
    }

    /* Step 5: Initialize and start HTTP server (web dashboard) */
    if (!http_server_init(&g_server, g_port)) {
        fprintf(stderr, "vekd: failed to initialize HTTP server\n");
        vekd_db_close(&g_db);
        return 1;
    }

    /* Setup web UI routes */
    VekdWebContext web_ctx = {
        .db = &g_db,
        .supervisor = &g_supervisor,
        .master_key = g_master_key,
        .next_port = VEKD_APP_PORT_MIN
    };
    vekd_web_init(&g_server, &web_ctx);

    printf("vekd: starting web dashboard on port %d\n", g_port);
    printf("vekd: visit http://localhost:%d to access the dashboard\n", g_port);

    /* Check if first boot (no users) */
    int user_count = vekd_db_user_count(&g_db);
    if (user_count == 0) {
        printf("vekd: first boot detected - create your admin account at the login page\n");
    }

    /* Start the HTTP server event loop (blocks until stop) */
    if (!http_server_start(&g_server)) {
        fprintf(stderr, "vekd: failed to start HTTP server on port %d\n", g_port);
        http_server_destroy(&g_server);
        vekd_db_close(&g_db);
        return 1;
    }

    /* Shutdown */
    printf("\nvekd: shutting down...\n");
    http_server_destroy(&g_server);
    vekd_supervisor_shutdown(&g_supervisor);
    vekd_db_close(&g_db);
    printf("vekd: stopped\n");

    return 0;
}
