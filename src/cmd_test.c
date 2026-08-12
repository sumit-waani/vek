#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "cli.h"
#include "vm.h"
#include "gc.h"
#include "memory.h"
#include "vek_stdlib.h"

#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

// ---- File discovery ----

typedef struct {
    char** paths;
    int count;
    int capacity;
} FileList;

static void filelist_init(FileList* fl) {
    fl->paths = NULL;
    fl->count = 0;
    fl->capacity = 0;
}

static void filelist_add(FileList* fl, const char* path) {
    if (fl->count >= fl->capacity) {
        int new_cap = fl->capacity < 8 ? 8 : fl->capacity * 2;
        fl->paths = (char**)realloc(fl->paths, sizeof(char*) * (size_t)new_cap);
        fl->capacity = new_cap;
    }
    fl->paths[fl->count] = strdup(path);
    fl->count++;
}

static void filelist_free(FileList* fl) {
    for (int i = 0; i < fl->count; i++) {
        free(fl->paths[i]);
    }
    free(fl->paths);
    fl->paths = NULL;
    fl->count = 0;
    fl->capacity = 0;
}

static bool should_skip_dir(const char* name) {
    return (strcmp(name, ".git") == 0 ||
            strcmp(name, "build") == 0 ||
            strcmp(name, "node_modules") == 0 ||
            strcmp(name, ".") == 0 ||
            strcmp(name, "..") == 0);
}

static bool ends_with_test_ve(const char* path) {
    size_t len = strlen(path);
    const char* suffix = ".test.ve";
    size_t suf_len = strlen(suffix);
    if (len < suf_len) return false;
    return strcmp(path + len - suf_len, suffix) == 0;
}

static void discover_test_files(const char* dir, FileList* fl) {
    DIR* d = opendir(dir);
    if (d == NULL) return;

    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        if (should_skip_dir(entry->d_name)) continue;

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);

        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            discover_test_files(path, fl);
        } else if (S_ISREG(st.st_mode) && ends_with_test_ve(entry->d_name)) {
            filelist_add(fl, path);
        }
    }
    closedir(d);
}

// Sort comparison for qsort
static int path_compare(const void* a, const void* b) {
    return strcmp(*(const char**)a, *(const char**)b);
}

// ---- File reading ----

static char* test_read_file(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) return NULL;
    fseek(file, 0L, SEEK_END);
    size_t file_size = (size_t)ftell(file);
    rewind(file);
    char* buffer = (char*)malloc(file_size + 1);
    if (buffer == NULL) { fclose(file); return NULL; }
    size_t bytes_read = fread(buffer, 1, file_size, file);
    if (bytes_read < file_size) { free(buffer); fclose(file); return NULL; }
    buffer[bytes_read] = '\0';
    fclose(file);
    return buffer;
}

// ---- Test execution ----

typedef struct {
    int passed;
    int failed;
    int total;
    bool verbose;
    const char* filter;
} TestStats;

static bool matches_filter(const char* name, const char* filter) {
    if (filter == NULL || filter[0] == '\0') return true;
    return strstr(name, filter) != NULL;
}

static void run_test_file(const char* path, TestStats* stats) {
    bool color = cli_color_enabled();

    // Print file header
    if (color) {
        printf("\n%s%s%s\n", CLI_BOLD, path, CLI_RESET);
    } else {
        printf("\n%s\n", path);
    }

    // Initialize VM for this file
    char* fake_argv[] = { "vek", "test", NULL };
    cli_set_args(2, fake_argv);
    gc_init();
    intern_table_init();
    heap_init();
    vm_init();
    stdlib_init();

    // Read and interpret the test file (this defines global test_* functions)
    char* source = test_read_file(path);
    if (source == NULL) {
        fprintf(stderr, "  Error: could not read file '%s'\n", path);
        vm_free();
        intern_table_destroy();
        gc_destroy();
        heap_destroy();
        return;
    }

    InterpretResult result = vm_interpret(source);
    free(source);

    if (result == INTERPRET_COMPILE_ERROR) {
        if (color) {
            printf("  %sCOMPILE ERROR%s\n", CLI_RED, CLI_RESET);
        } else {
            printf("  COMPILE ERROR\n");
        }
        stats->failed++;
        stats->total++;
        vm_free();
        intern_table_destroy();
        gc_destroy();
        heap_destroy();
        return;
    }

    if (result == INTERPRET_RUNTIME_ERROR) {
        if (color) {
            printf("  %sRUNTIME ERROR%s (during file load)\n", CLI_RED, CLI_RESET);
        } else {
            printf("  RUNTIME ERROR (during file load)\n");
        }
        stats->failed++;
        stats->total++;
        vm_free();
        intern_table_destroy();
        gc_destroy();
        heap_destroy();
        return;
    }

    // Discover test_* functions in globals
    typedef struct {
        ObjString* name;
        Value closure;
    } TestFunc;

    TestFunc* tests = NULL;
    int test_count = 0;
    int test_cap = 0;

    for (uint32_t i = 0; i < vm.globals->capacity; i++) {
        MapEntry* entry = &vm.globals->entries[i];
        if (entry->key == NULL) continue;
        // Check for tombstone
        if ((void*)entry->key == (void*)MAP_TOMBSTONE) continue;

        // Check if name starts with "test_"
        if (entry->key->length < 5) continue;
        if (memcmp(entry->key->data, "test_", 5) != 0) continue;

        // Check if value is a closure
        if (!IS_CLOSURE(entry->value)) continue;

        // Apply filter
        if (!matches_filter(entry->key->data, stats->filter)) continue;

        // Add to test list
        if (test_count >= test_cap) {
            test_cap = test_cap < 8 ? 8 : test_cap * 2;
            tests = (TestFunc*)realloc(tests, sizeof(TestFunc) * (size_t)test_cap);
        }
        tests[test_count].name = entry->key;
        tests[test_count].closure = entry->value;
        test_count++;
    }

    // Sort tests by name for deterministic order
    for (int i = 0; i < test_count - 1; i++) {
        for (int j = i + 1; j < test_count; j++) {
            int cmp = strcmp(tests[i].name->data, tests[j].name->data);
            if (cmp > 0) {
                TestFunc tmp = tests[i];
                tests[i] = tests[j];
                tests[j] = tmp;
            }
        }
    }

    // Run each test function
    for (int i = 0; i < test_count; i++) {
        const char* name = tests[i].name->data;

        // Reset error state
        vm.had_error = false;
        vm.error_msg[0] = '\0';

        // Push the closure and call with 0 args
        vm_push(tests[i].closure);
        vm_call(tests[i].closure, 0);

        stats->total++;

        if (vm.had_error) {
            stats->failed++;
            if (color) {
                printf("  %s\xe2\x9c\x97 %s%s", CLI_RED, name, CLI_RESET);
            } else {
                printf("  FAIL %s", name);
            }
            if (vm.error_msg[0] != '\0') {
                printf(" - %s", vm.error_msg);
            }
            printf("\n");
        } else {
            stats->passed++;
            if (stats->verbose) {
                if (color) {
                    printf("  %s\xe2\x9c\x93 %s%s\n", CLI_GREEN, name, CLI_RESET);
                } else {
                    printf("  PASS %s\n", name);
                }
            }
        }
    }

    if (test_count == 0) {
        if (color) {
            printf("  %s(no test functions found)%s\n", CLI_DIM, CLI_RESET);
        } else {
            printf("  (no test functions found)\n");
        }
    }

    free(tests);

    // Cleanup VM
    vm_free();
    intern_table_destroy();
    gc_destroy();
    heap_destroy();
}

// ---- Public entry point ----

int cmd_test_run(int argc, char** argv) {
    if (cli_has_flag(argc, argv, "--help")) {
        printf("Usage: vek test [files...] [options]\n");
        printf("\n");
        printf("Run test files (.test.ve).\n");
        printf("\n");
        printf("Options:\n");
        printf("  --verbose          Print each passing assertion\n");
        printf("  --filter=<pattern> Only run tests matching substring\n");
        printf("  --help             Show this help message\n");
        printf("\n");
        printf("If no files are specified, recursively finds all *.test.ve files\n");
        printf("in the current directory.\n");
        return 0;
    }

    TestStats stats = {0, 0, 0, false, NULL};
    stats.verbose = cli_has_flag(argc, argv, "--verbose");

    // Parse --filter=<pattern>
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--filter=", 9) == 0) {
            stats.filter = argv[i] + 9;
            break;
        }
    }

    // Collect file arguments (non-flag args after "test")
    FileList files;
    filelist_init(&files);

    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) == 0) continue;
        filelist_add(&files, argv[i]);
    }

    // If no files specified, discover them
    if (files.count == 0) {
        discover_test_files(".", &files);
        // Sort for deterministic order
        if (files.count > 1) {
            qsort(files.paths, (size_t)files.count, sizeof(char*), path_compare);
        }
    }

    if (files.count == 0) {
        printf("No test files found.\n");
        filelist_free(&files);
        return 0;
    }

    // Run each test file
    for (int i = 0; i < files.count; i++) {
        run_test_file(files.paths[i], &stats);
    }

    // Print summary
    bool color = cli_color_enabled();
    printf("\n");

    if (stats.failed == 0) {
        if (color) {
            printf("%s%d passed%s, %d total\n", CLI_GREEN, stats.passed, CLI_RESET, stats.total);
        } else {
            printf("%d passed, 0 failed, %d total\n", stats.passed, stats.total);
        }
    } else {
        if (color) {
            printf("%s%d passed%s, %s%d failed%s, %d total\n",
                   CLI_GREEN, stats.passed, CLI_RESET,
                   CLI_RED, stats.failed, CLI_RESET,
                   stats.total);
        } else {
            printf("%d passed, %d failed, %d total\n", stats.passed, stats.failed, stats.total);
        }
    }

    filelist_free(&files);
    return stats.failed > 0 ? 1 : 0;
}
