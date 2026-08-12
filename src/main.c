#include "common.h"
#include "memory.h"
#include "gc.h"
#include "object.h"
#include "vm.h"
#include "vek_stdlib.h"

#include <errno.h>

// Read an entire file into a heap-allocated string.
// Returns NULL on failure.
static char* read_file(const char* path) {
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

// Run a source file
static int run_file(const char* path) {
    char* source = read_file(path);
    if (source == NULL) return 74; // EX_IOERR

    InterpretResult result = vm_interpret(source);
    free(source);

    if (result == INTERPRET_COMPILE_ERROR) return 65; // EX_DATAERR
    if (result == INTERPRET_RUNTIME_ERROR) return 70; // EX_SOFTWARE
    return 0;
}

// Basic REPL
static void repl(void) {
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
}

static void print_usage(void) {
    printf("vek %s\n", VEK_VERSION_STRING);
    printf("Usage: vek [command] [args]\n");
    printf("\n");
    printf("Commands:\n");
    printf("  run <file.ve>    Run a vek source file\n");
    printf("  repl             Start interactive REPL\n");
    printf("  --version        Print version information\n");
    printf("  --help           Print this help message\n");
    printf("\n");
    printf("If no command is given, this help text is displayed.\n");
}

int main(int argc, char* argv[]) {
    // Handle --version flag (before initializing subsystems)
    if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
        printf("vek %s\n", VEK_VERSION_STRING);
        return 0;
    }

    // Handle --help
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        print_usage();
        return 0;
    }

    // Initialize subsystems
    gc_init();
    intern_table_init();
    heap_init();
    vm_init();
    stdlib_init();

    int exit_code = 0;

    if (argc < 2) {
        print_usage();
    } else if (strcmp(argv[1], "run") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: 'run' command requires a file argument.\n");
            fprintf(stderr, "Usage: vek run <file.ve>\n");
            exit_code = 64; // EX_USAGE
        } else {
            exit_code = run_file(argv[2]);
        }
    } else if (strcmp(argv[1], "repl") == 0) {
        repl();
    } else {
        // Try to run the argument as a file (convenience)
        exit_code = run_file(argv[1]);
    }

    // Cleanup
    vm_free();
    intern_table_destroy();
    gc_destroy();
    heap_destroy();

    return exit_code;
}
