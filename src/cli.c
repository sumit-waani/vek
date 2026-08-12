#include "common.h"
#include "cli.h"
#include "memory.h"
#include "gc.h"
#include "object.h"
#include "vm.h"
#include "vek_stdlib.h"

#include <unistd.h>
#include <errno.h>

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

// ---- Command handlers ----

static int cmd_run(int argc, char** argv) {
    if (cli_has_flag(argc, argv, "--help")) {
        printf("Usage: vek run <file.ve> [args...]\n");
        printf("\n");
        printf("Run a vek source file.\n");
        return 0;
    }

    // argv[0] = "vek", argv[1] = "run", argv[2] = file
    if (argc < 3) {
        fprintf(stderr, "Error: 'run' command requires a file argument.\n");
        fprintf(stderr, "Usage: vek run <file.ve>\n");
        return 64; // EX_USAGE
    }

    const char* path = argv[2];

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

static int cmd_repl(int argc, char** argv) {
    if (cli_has_flag(argc, argv, "--help")) {
        printf("Usage: vek repl\n");
        printf("\n");
        printf("Start interactive REPL.\n");
        return 0;
    }

    vm_init_all(argc, argv);

    char line[4096];
    printf("vek %s REPL (type 'exit' or Ctrl-D to quit)\n", VEK_VERSION_STRING);

    for (;;) {
        printf(">> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;
        }

        // Check for exit
        if (strncmp(line, "exit", 4) == 0 && (line[4] == '\n' || line[4] == '\0')) {
            break;
        }

        vm_interpret(line);
    }

    vm_cleanup_all();
    return 0;
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
    if (cli_has_flag(argc, argv, "--help")) {
        printf("Usage: vek fmt [files...]\n");
        printf("\n");
        printf("Format vek source files.\n");
        return 0;
    }
    printf("vek fmt: not yet implemented\n");
    return 0;
}

static int cmd_shell(int argc, char** argv) {
    if (cli_has_flag(argc, argv, "--help")) {
        printf("Usage: vek shell [options]\n");
        printf("\n");
        printf("Interactive shell with app context loaded.\n");
        return 0;
    }
    printf("vek shell: not yet implemented\n");
    return 0;
}

static int cmd_migrate(int argc, char** argv) {
    if (cli_has_flag(argc, argv, "--help")) {
        printf("Usage: vek migrate [options]\n");
        printf("\n");
        printf("Run database migrations.\n");
        return 0;
    }
    printf("vek migrate: not yet implemented\n");
    return 0;
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
    {"run",     "Run a vek source file",                    "vek run <file.ve> [args...]",  cmd_run},
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
