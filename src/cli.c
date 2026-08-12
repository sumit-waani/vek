#include "common.h"
#include "cli.h"
#include "memory.h"
#include "gc.h"
#include "object.h"
#include "vm.h"
#include "vek_stdlib.h"
#include "vebc_loader.h"

#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <signal.h>

// ---- Internal helpers ----

static char* cli_read_file(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "Error: Could not open file '%s': %s\n", path, strerror(errno));
        return NULL;
    }

    fseek(file, 0L, SEEK_END);
    size_t file_size = (size_t)ftell(file);
    rewind(file);

    char* buffer = (char*)malloc(file_size + 1);
    if (buffer == NULL) {
        fprintf(stderr, "Error: Not enough memory to read '%s'.\n", path);
        fclose(file);
        return NULL;
    }

    size_t bytes_read = fread(buffer, 1, file_size, file);
    if (bytes_read < file_size) {
        fprintf(stderr, "Error: Could not read file '%s'.\n", path);
        free(buffer);
        fclose(file);
        return NULL;
    }

    buffer[bytes_read] = '\0';
    fclose(file);
    return buffer;
}

static void vm_init_all(int argc, char** argv) {
    cli_set_args(argc, argv);
    gc_init();
    intern_table_init();
    heap_init();
    vm_init();
    stdlib_init();
}

static void vm_cleanup_all(void) {
    vm_free();
    intern_table_destroy();
    gc_destroy();
    heap_destroy();
}

// ---- Run command helpers ----

// Parse --workers=N or --workers N, returns 1 if not specified
static int parse_workers(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--workers=", 10) == 0) {
            int w = atoi(argv[i] + 10);
            if (w > 0) return w;
        }
    }
    const char* val = cli_get_option(argc, argv, "--workers");
    if (val != NULL) {
        int w = atoi(val);
        if (w > 0) return w;
    }
    return 1;
}

// Parse --port=N or --port N, returns 0 if not specified
static int parse_run_port(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--port=", 7) == 0) {
            int p = atoi(argv[i] + 7);
            if (p > 0 && p < 65536) return p;
        }
    }
    const char* val = cli_get_option(argc, argv, "--port");
    if (val != NULL) {
        int p = atoi(val);
        if (p > 0 && p < 65536) return p;
    }
    return 0;
}

// Find the file argument (first non-flag argument after "run")
static const char* find_run_file(int argc, char** argv) {
    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) == 0) {
            // Skip --key=val style
            if (strchr(argv[i], '=') != NULL) continue;
            // Skip --key val style (consume next arg)
            if (strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "--workers") == 0) {
                i++; // skip the value
                continue;
            }
            continue;
        }
        return argv[i];
    }
    return NULL;
}

// Check if a file has a given extension
static bool has_extension(const char* path, const char* ext) {
    size_t plen = strlen(path);
    size_t elen = strlen(ext);
    if (plen < elen) return false;
    return strcmp(path + plen - elen, ext) == 0;
}

// Run a single .ve source file
static int run_source_file(const char* path, int argc, char** argv) {
    vm_init_all(argc, argv);

    char* source = cli_read_file(path);
    if (source == NULL) {
        vm_cleanup_all();
        return 74; // EX_IOERR
    }

    InterpretResult result = vm_interpret(source);
    free(source);

    int exit_code = 0;
    if (result == INTERPRET_COMPILE_ERROR) exit_code = 65; // EX_DATAERR
    if (result == INTERPRET_RUNTIME_ERROR) exit_code = 70; // EX_SOFTWARE

    vm_cleanup_all();
    return exit_code;
}

// Run a .vebc bytecode file
static int run_vebc_file(const char* path, int argc, char** argv) {
    VebcFile* vf = vebc_load(path);
    if (vf == NULL) {
        return 74; // EX_IOERR
    }

    if (!vebc_verify(vf)) {
        fprintf(stderr, "Error: integrity check failed for '%s'\n", path);
        vebc_free(vf);
        return 65; // EX_DATAERR
    }

    vm_init_all(argc, argv);

    ObjFunction* function = vebc_to_function(vf);
    if (function == NULL) {
        vm_cleanup_all();
        vebc_free(vf);
        return 65; // EX_DATAERR
    }

    // Execute like vm_interpret does: wrap in closure and call
    ObjClosure* closure = obj_closure_new(function);
    vm_push(OBJ_VAL(closure));
    Value callee = vm_pop();
    vm_push(callee);
    Value call_result = vm_call(callee, 0);
    (void)call_result;

    int exit_code = 0;
    if (vm.had_error) exit_code = 70; // EX_SOFTWARE

    vm_cleanup_all();
    vebc_free(vf);
    return exit_code;
}

// Worker loop: fork N children, monitor and restart on crash
static int run_with_workers(int workers, const char* path, bool is_vebc, int argc, char** argv) {
    pid_t* pids = (pid_t*)calloc((size_t)workers, sizeof(pid_t));
    if (!pids) {
        fprintf(stderr, "Error: could not allocate worker array\n");
        return 1;
    }

    // Fork initial workers
    for (int i = 0; i < workers; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "Error: fork failed: %s\n", strerror(errno));
            free(pids);
            return 1;
        }
        if (pid == 0) {
            // Child process - run the app
            int rc;
            if (is_vebc) {
                rc = run_vebc_file(path, argc, argv);
            } else {
                rc = run_source_file(path, argc, argv);
            }
            _exit(rc);
        }
        pids[i] = pid;
        fprintf(stderr, "[worker %d] started (pid %d)\n", i, pid);
    }

    // Parent: monitor children and restart on crash
    for (;;) {
        int status;
        pid_t exited = waitpid(-1, &status, 0);
        if (exited < 0) {
            if (errno == EINTR) continue;
            break; // no more children
        }

        // Find which worker died
        int worker_idx = -1;
        for (int i = 0; i < workers; i++) {
            if (pids[i] == exited) {
                worker_idx = i;
                break;
            }
        }

        if (worker_idx < 0) continue;

        if (WIFSIGNALED(status)) {
            fprintf(stderr, "[worker %d] crashed (signal %d), restarting...\n",
                    worker_idx, WTERMSIG(status));
        } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            fprintf(stderr, "[worker %d] exited with code %d, restarting...\n",
                    worker_idx, WEXITSTATUS(status));
        } else {
            // Normal exit
            fprintf(stderr, "[worker %d] exited normally\n", worker_idx);
            pids[worker_idx] = 0;
            // Check if all workers have exited normally
            bool all_done = true;
            for (int i = 0; i < workers; i++) {
                if (pids[i] != 0) { all_done = false; break; }
            }
            if (all_done) break;
            continue;
        }

        // Restart the crashed worker
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "Error: fork failed during restart: %s\n", strerror(errno));
            break;
        }
        if (pid == 0) {
            int rc;
            if (is_vebc) {
                rc = run_vebc_file(path, argc, argv);
            } else {
                rc = run_source_file(path, argc, argv);
            }
            _exit(rc);
        }
        pids[worker_idx] = pid;
        fprintf(stderr, "[worker %d] restarted (pid %d)\n", worker_idx, pid);
    }

    // Clean up remaining children
    for (int i = 0; i < workers; i++) {
        if (pids[i] > 0) {
            kill(pids[i], SIGTERM);
            waitpid(pids[i], NULL, 0);
        }
    }

    free(pids);
    return 0;
}

// ---- Command handlers ----

static int cmd_run(int argc, char** argv) {
    if (cli_has_flag(argc, argv, "--help")) {
        printf("Usage: vek run <file> [options] [args...]\n");
        printf("\n");
        printf("Run a vek source file or compiled .vebc artifact.\n");
        printf("\n");
        printf("Options:\n");
        printf("  --port=N       Set PORT environment variable for the app\n");
        printf("  --workers=N    Fork N worker processes (default: 1)\n");
        printf("  --help         Show this help message\n");
        printf("\n");
        printf("If <file> has no extension, tries .vebc first, then .ve.\n");
        return 0;
    }

    const char* file_arg = find_run_file(argc, argv);
    if (file_arg == NULL) {
        fprintf(stderr, "Error: 'run' command requires a file argument.\n");
        fprintf(stderr, "Usage: vek run <file> [options]\n");
        return 64; // EX_USAGE
    }

    int port = parse_run_port(argc, argv);
    int workers = parse_workers(argc, argv);

    // Set PORT env var if specified
    if (port > 0) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port);
        setenv("PORT", port_str, 1);
    }

    // Determine the actual file path and type
    char resolved_path[4096];
    bool is_vebc = false;

    if (has_extension(file_arg, ".vebc")) {
        // Explicit .vebc file
        snprintf(resolved_path, sizeof(resolved_path), "%s", file_arg);
        is_vebc = true;
    } else if (has_extension(file_arg, ".ve")) {
        // Explicit .ve file
        snprintf(resolved_path, sizeof(resolved_path), "%s", file_arg);
        is_vebc = false;
    } else {
        // No extension: try .vebc first, then .ve
        snprintf(resolved_path, sizeof(resolved_path), "%s.vebc", file_arg);
        if (access(resolved_path, R_OK) == 0) {
            is_vebc = true;
        } else {
            snprintf(resolved_path, sizeof(resolved_path), "%s.ve", file_arg);
            if (access(resolved_path, R_OK) != 0) {
                // Try the raw path as given
                snprintf(resolved_path, sizeof(resolved_path), "%s", file_arg);
                is_vebc = false;
            }
        }
    }

    // Run with workers if requested
    if (workers > 1) {
        return run_with_workers(workers, resolved_path, is_vebc, argc, argv);
    }

    // Single process run
    if (is_vebc) {
        return run_vebc_file(resolved_path, argc, argv);
    } else {
        return run_source_file(resolved_path, argc, argv);
    }
}

static int cmd_repl(int argc, char** argv) {
    return cmd_repl_run(argc, argv);
}
static int cmd_new(int argc, char** argv) {
    if (cli_has_flag(argc, argv, "--help")) {
        printf("Usage: vek new <project-name> [options]\n");
        printf("\n");
        printf("Create a new vek project with the standard directory structure.\n");
        printf("\n");
        printf("Arguments:\n");
        printf("  <project-name>    Name or path for the new project\n");
        printf("\n");
        printf("Options:\n");
        printf("  --no-prompt       Skip interactive prompts (fail if dir exists)\n");
        printf("  --help            Show this help message\n");
        printf("\n");
        printf("Creates:\n");
        printf("  <project-name>/\n");
        printf("    app.ve              Main entry point\n");
        printf("    routes/index.ve     Route definitions\n");
        printf("    views/index.ve      View templates\n");
        printf("    views/layouts/      Layout templates\n");
        printf("    public/css/         Static CSS files\n");
        printf("    public/js/          Static JS files\n");
        printf("    migrations/         Database migrations\n");
        printf("    config/app.ve       Application config\n");
        return 0;
    }
    return cmd_new_run(argc, argv);
}

static int cmd_dev(int argc, char** argv) {
    if (cli_has_flag(argc, argv, "--help")) {
        printf("Usage: vek dev [options]\n");
        printf("\n");
        printf("Start development server with hot reload.\n");
        printf("\n");
        printf("Options:\n");
        printf("  --port=N    Port to listen on (default: 3000, or PORT env var)\n");
        printf("  --help      Show this help message\n");
        printf("\n");
        printf("Watches for .ve file changes and automatically restarts the app.\n");
        printf("Looks for app.ve in the current directory.\n");
        return 0;
    }
    return cmd_dev_run(argc, argv);
}

static int cmd_build(int argc, char** argv) {
    if (cli_has_flag(argc, argv, "--help")) {
        printf("Usage: vek build [options]\n");
        printf("\n");
        printf("Compile all .ve source files and package into a .vebc binary artifact.\n");
        printf("\n");
        printf("Options:\n");
        printf("  --output=<path>   Output file path (default: build/<appname>.vebc)\n");
        printf("  -o <path>         Short form of --output\n");
        printf("  --help            Show this help message\n");
        printf("\n");
        printf("Compiles app.ve and all .ve files in routes/, embeds public/ assets,\n");
        printf("and produces a single .vebc binary for deployment.\n");
        return 0;
    }
    return cmd_build_run(argc, argv);
}

static int cmd_fmt(int argc, char** argv) {
    return cmd_fmt_run(argc, argv);
}

static int cmd_shell(int argc, char** argv) {
    return cmd_shell_run(argc, argv);
}

static int cmd_migrate(int argc, char** argv) {
    return cmd_migrate_run(argc, argv);
}

static int cmd_test(int argc, char** argv) {
    if (cli_has_flag(argc, argv, "--help")) {
        printf("Usage: vek test [files...]\n");
        printf("\n");
        printf("Run test files.\n");
        return 0;
    }
    printf("vek test: not yet implemented\n");
    return 0;
}

// ---- Command registry ----

static Command commands[] = {
    {"run",     "Run a vek source file or .vebc artifact",  "vek run <file> [options]",     cmd_run},
    {"repl",    "Start interactive REPL",                   "vek repl",                     cmd_repl},
    {"new",     "Create a new vek project",                 "vek new <project-name>",       cmd_new},
    {"dev",     "Start development server with hot reload", "vek dev [options]",            cmd_dev},
    {"build",   "Build .vebc artifact for deployment",      "vek build [options]",          cmd_build},
    {"fmt",     "Format vek source files",                  "vek fmt [files...]",           cmd_fmt},
    {"shell",   "Interactive shell with app context loaded","vek shell [options]",          cmd_shell},
    {"migrate", "Run database migrations",                  "vek migrate [options]",        cmd_migrate},
    {"test",    "Run test files",                           "vek test [files...]",          cmd_test},
};

static const int command_count = (int)(sizeof(commands) / sizeof(commands[0]));

// ---- Public API ----

bool cli_color_enabled(void) {
    // Disable color if NO_COLOR env is set (any value)
    if (getenv("NO_COLOR") != NULL) {
        return false;
    }
    // Disable color if stdout is not a terminal
    if (!isatty(STDOUT_FILENO)) {
        return false;
    }
    return true;
}

bool cli_has_flag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], flag) == 0) {
            return true;
        }
    }
    return false;
}

const char* cli_get_option(int argc, char** argv, const char* key) {
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], key) == 0) {
            return argv[i + 1];
        }
    }
    return NULL;
}

void cli_print_help(void) {
    bool color = cli_color_enabled();

    if (color) {
        printf("%s%svek%s %s\n", CLI_BOLD, CLI_CYAN, CLI_RESET, VEK_VERSION_STRING);
    } else {
        printf("vek %s\n", VEK_VERSION_STRING);
    }

    printf("A modern scripting language for web development\n");
    printf("\n");

    if (color) {
        printf("%sUsage:%s vek [command] [args]\n", CLI_BOLD, CLI_RESET);
    } else {
        printf("Usage: vek [command] [args]\n");
    }

    printf("\n");

    if (color) {
        printf("%sCommands:%s\n", CLI_BOLD, CLI_RESET);
    } else {
        printf("Commands:\n");
    }

    for (int i = 0; i < command_count; i++) {
        if (color) {
            printf("  %s%-10s%s %s\n", CLI_GREEN, commands[i].name, CLI_RESET, commands[i].description);
        } else {
            printf("  %-10s %s\n", commands[i].name, commands[i].description);
        }
    }

    printf("\n");

    if (color) {
        printf("%sOptions:%s\n", CLI_BOLD, CLI_RESET);
    } else {
        printf("Options:\n");
    }

    printf("  --version    Print version information\n");
    printf("  --help       Print this help message\n");
    printf("\n");
    printf("Run 'vek help <command>' for more information on a specific command.\n");
}

int cli_dispatch(int argc, char** argv) {
    if (argc < 2) {
        cli_print_help();
        return 0;
    }

    const char* cmd_name = argv[1];

    // Handle 'help' as a special command
    if (strcmp(cmd_name, "help") == 0) {
        if (argc >= 3) {
            // 'vek help <cmd>' - show per-command help
            const char* target = argv[2];
            for (int i = 0; i < command_count; i++) {
                if (strcmp(commands[i].name, target) == 0) {
                    // Simulate --help for that command
                    char* help_argv[] = {argv[0], (char*)target, "--help", NULL};
                    return commands[i].handler(3, help_argv);
                }
            }
            fprintf(stderr, "Error: unknown command '%s'. Run 'vek --help' for usage.\n", target);
            return 1;
        }
        cli_print_help();
        return 0;
    }

    // Look up command
    for (int i = 0; i < command_count; i++) {
        if (strcmp(commands[i].name, cmd_name) == 0) {
            return commands[i].handler(argc, argv);
        }
    }

    // Unknown command
    fprintf(stderr, "Error: unknown command '%s'. Run 'vek --help' for usage.\n", cmd_name);
    return 1;
}
