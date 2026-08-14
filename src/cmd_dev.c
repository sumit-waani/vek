#define _GNU_SOURCE
#include "common.h"
#include "cli.h"
#include "file_watcher.h"

#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <limits.h>

#define DEFAULT_PORT 3000

// Global state for signal handling
static volatile pid_t g_child_pid = 0;
static volatile sig_atomic_t g_should_exit = 0;

static void sigint_handler(int sig) {
    (void)sig;
    g_should_exit = 1;
}

static void sigchld_handler(int sig) {
    (void)sig;
    // Just let the main loop handle it via waitpid
}

// Get the path to the current vek binary (for re-exec)
static bool get_vek_path(char* buf, size_t size) {
    ssize_t len = readlink("/proc/self/exe", buf, size - 1);
    if (len < 0) {
        return false;
    }
    buf[len] = '\0';
    return true;
}

// Parse port from --port=N or --port N style arguments, or PORT env var
static int parse_port(int argc, char** argv) {
    // Check --port=N format
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--port=", 7) == 0) {
            int port = atoi(argv[i] + 7);
            if (port > 0 && port < 65536) return port;
        }
    }

    // Check --port N format
    const char* port_str = cli_get_option(argc, argv, "--port");
    if (port_str != NULL) {
        int port = atoi(port_str);
        if (port > 0 && port < 65536) return port;
    }

    // Check PORT environment variable
    const char* env_port = getenv("PORT");
    if (env_port != NULL) {
        int port = atoi(env_port);
        if (port > 0 && port < 65536) return port;
    }

    return DEFAULT_PORT;
}

// Fork a child process to run the app
static pid_t start_child(const char* vek_path, int port) {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        // Child process
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port);
        setenv("PORT", port_str, 1);

        execl(vek_path, "vek", "run", "app.ve", NULL);
        // If execl returns, it failed
        fprintf(stderr, "Error: failed to exec vek: %s\n", strerror(errno));
        _exit(127);
    }

    return pid;
}

// Kill the child process and wait for it to exit
static void stop_child(pid_t pid) {
    if (pid <= 0) return;

    kill(pid, SIGTERM);

    // Wait up to 2 seconds for graceful shutdown
    int status;
    for (int i = 0; i < 20; i++) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result > 0) return;
        if (result < 0) return;
        usleep(100000); // 100ms
    }

    // Force kill if still running
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
}

// Check if child has exited (non-blocking)
static int check_child(pid_t pid, int* exit_status) {
    if (pid <= 0) return 0;

    int status;
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result > 0) {
        if (WIFEXITED(status)) {
            *exit_status = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            *exit_status = 128 + WTERMSIG(status);
        } else {
            *exit_status = 1;
        }
        return 1; // child exited
    }
    return 0; // still running
}

int cmd_dev_run(int argc, char** argv) {
    bool color = cli_color_enabled();

    // Check for app.ve in current directory
    if (access("app.ve", F_OK) != 0) {
        if (color) {
            fprintf(stderr, "%sError:%s No app.ve found in current directory.\n",
                    CLI_RED, CLI_RESET);
        } else {
            fprintf(stderr, "Error: No app.ve found in current directory.\n");
        }
        fprintf(stderr, "Run 'vek new <name>' to create a new project, or create app.ve.\n");
        return 1;
    }

    // Get vek binary path for re-exec
    char vek_path[PATH_MAX];
    if (!get_vek_path(vek_path, sizeof(vek_path))) {
        fprintf(stderr, "Error: could not determine vek binary path.\n");
        return 1;
    }

    int port = parse_port(argc, argv);

    // Print startup banner
    if (color) {
        printf("%s%svek dev%s | http://localhost:%d\n",
               CLI_BOLD, CLI_GREEN, CLI_RESET, port);
        printf("%sWatching for .ve file changes...%s\n", CLI_DIM, CLI_RESET);
    } else {
        printf("vek dev | http://localhost:%d\n", port);
        printf("Watching for .ve file changes...\n");
    }
    fflush(stdout);

    // Set up file watcher
    FileWatcher* fw = file_watcher_create();
    if (fw == NULL) {
        fprintf(stderr, "Error: could not initialize file watcher.\n");
        return 1;
    }

    if (!file_watcher_add_dir(fw, ".")) {
        fprintf(stderr, "Error: could not watch current directory.\n");
        file_watcher_destroy(fw);
        return 1;
    }

    // Set up signal handlers
    struct sigaction sa_int;
    memset(&sa_int, 0, sizeof(sa_int));
    sa_int.sa_handler = sigint_handler;
    sigemptyset(&sa_int.sa_mask);
    sigaction(SIGINT, &sa_int, NULL);
    sigaction(SIGTERM, &sa_int, NULL);

    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags = SA_NOCLDSTOP;
    sigemptyset(&sa_chld.sa_mask);
    sigaction(SIGCHLD, &sa_chld, NULL);

    // Start the child process
    g_child_pid = start_child(vek_path, port);
    if (g_child_pid < 0) {
        fprintf(stderr, "Error: could not start app process.\n");
        file_watcher_destroy(fw);
        return 1;
    }

    bool child_crashed = false;

    // Main dev loop
    while (!g_should_exit) {
        char changed_path[4096];
        int result = file_watcher_poll(fw, changed_path, sizeof(changed_path), 500);

        if (g_should_exit) break;

        // Check if child exited unexpectedly
        if (!child_crashed && g_child_pid > 0) {
            int exit_status = 0;
            if (check_child(g_child_pid, &exit_status)) {
                if (exit_status != 0) {
                    child_crashed = true;
                    g_child_pid = 0;
                    if (color) {
                        printf("\n%s[error]%s Process exited with code %d. Waiting for file change to restart...\n",
                               CLI_RED, CLI_RESET, exit_status);
                    } else {
                        printf("\n[error] Process exited with code %d. Waiting for file change to restart...\n",
                               exit_status);
                    }
                    fflush(stdout);
                } else {
                    // Clean exit - also wait for file change
                    g_child_pid = 0;
                }
            }
        }

        if (result == 1) {
            // File changed - reload
            if (color) {
                printf("%s[reload]%s %s%s%s changed\n",
                       CLI_CYAN, CLI_RESET, CLI_BOLD, changed_path, CLI_RESET);
            } else {
                printf("[reload] %s changed\n", changed_path);
            }
            fflush(stdout);

            // Stop current child if running
            if (g_child_pid > 0) {
                stop_child(g_child_pid);
                g_child_pid = 0;
            }

            // Restart
            child_crashed = false;
            g_child_pid = start_child(vek_path, port);
            if (g_child_pid < 0) {
                if (color) {
                    fprintf(stderr, "%s[error]%s Could not restart app process.\n",
                            CLI_RED, CLI_RESET);
                } else {
                    fprintf(stderr, "[error] Could not restart app process.\n");
                }
            }
        }
    }

    // Clean shutdown
    printf("\n");
    if (color) {
        printf("%sStopping dev server...%s\n", CLI_DIM, CLI_RESET);
    } else {
        printf("Stopping dev server...\n");
    }

    if (g_child_pid > 0) {
        stop_child(g_child_pid);
    }

    file_watcher_destroy(fw);
    return 0;
}
