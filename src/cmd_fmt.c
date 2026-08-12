#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "cli.h"
#include "formatter.h"

#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>

// ---- File I/O helpers ----

static char* read_file_contents(const char* path, size_t* out_size) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "Error: could not open '%s': %s\n", path, strerror(errno));
        return NULL;
    }

    fseek(file, 0L, SEEK_END);
    long file_size = ftell(file);
    rewind(file);

    if (file_size < 0) {
        fclose(file);
        return NULL;
    }

    char* buffer = (char*)malloc((size_t)file_size + 1);
    if (buffer == NULL) {
        fprintf(stderr, "Error: not enough memory to read '%s'\n", path);
        fclose(file);
        return NULL;
    }

    size_t bytes_read = fread(buffer, 1, (size_t)file_size, file);
    buffer[bytes_read] = '\0';
    fclose(file);

    if (out_size) *out_size = bytes_read;
    return buffer;
}

static bool write_file_contents(const char* path, const char* data, size_t length) {
    FILE* file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "Error: could not write '%s': %s\n", path, strerror(errno));
        return false;
    }

    size_t written = fwrite(data, 1, length, file);
    fclose(file);

    return written == length;
}

// ---- Simple diff output ----

static void print_diff(const char* path, const char* original, const char* formatted) {
    printf("--- %s (original)\n", path);
    printf("+++ %s (formatted)\n", path);

    const char* orig_p = original;
    const char* fmt_p = formatted;
    int line_num = 1;

    while (*orig_p || *fmt_p) {
        const char* orig_line_start = orig_p;
        while (*orig_p && *orig_p != '\n') orig_p++;
        size_t orig_len = (size_t)(orig_p - orig_line_start);
        if (*orig_p == '\n') orig_p++;

        const char* fmt_line_start = fmt_p;
        while (*fmt_p && *fmt_p != '\n') fmt_p++;
        size_t fmt_len = (size_t)(fmt_p - fmt_line_start);
        if (*fmt_p == '\n') fmt_p++;

        if (orig_len != fmt_len || memcmp(orig_line_start, fmt_line_start, orig_len) != 0) {
            printf("@@ line %d @@\n", line_num);
            printf("-%.*s\n", (int)orig_len, orig_line_start);
            printf("+%.*s\n", (int)fmt_len, fmt_line_start);
        }

        line_num++;

        if (!*orig_p && !*fmt_p) break;
        if (!*orig_p && *fmt_p) {
            printf("@@ line %d @@\n", line_num);
            while (*fmt_p) {
                const char* ls = fmt_p;
                while (*fmt_p && *fmt_p != '\n') fmt_p++;
                printf("+%.*s\n", (int)(fmt_p - ls), ls);
                if (*fmt_p == '\n') fmt_p++;
                line_num++;
            }
            break;
        }
        if (*orig_p && !*fmt_p) {
            printf("@@ line %d @@\n", line_num);
            while (*orig_p) {
                const char* ls = orig_p;
                while (*orig_p && *orig_p != '\n') orig_p++;
                printf("-%.*s\n", (int)(orig_p - ls), ls);
                if (*orig_p == '\n') orig_p++;
                line_num++;
            }
            break;
        }
    }
}

// ---- Recursive file discovery ----

typedef struct {
    char** paths;
    size_t count;
    size_t capacity;
} FileList;

static void filelist_init(FileList* fl) {
    fl->paths = NULL;
    fl->count = 0;
    fl->capacity = 0;
}

static void filelist_add(FileList* fl, const char* path) {
    if (fl->count >= fl->capacity) {
        fl->capacity = fl->capacity < 16 ? 16 : fl->capacity * 2;
        fl->paths = (char**)realloc(fl->paths, sizeof(char*) * fl->capacity);
    }
    fl->paths[fl->count++] = strdup(path);
}

static void filelist_free(FileList* fl) {
    for (size_t i = 0; i < fl->count; i++) {
        free(fl->paths[i]);
    }
    free(fl->paths);
    fl->paths = NULL;
    fl->count = 0;
    fl->capacity = 0;
}

static bool should_skip_dir(const char* name) {
    if (strcmp(name, ".git") == 0) return true;
    if (strcmp(name, "build") == 0) return true;
    if (strcmp(name, "node_modules") == 0) return true;
    if (strcmp(name, ".agents") == 0) return true;
    return false;
}

static bool has_ve_extension(const char* name) {
    size_t len = strlen(name);
    return len > 3 && strcmp(name + len - 3, ".ve") == 0;
}

static void find_ve_files(const char* dir_path, FileList* fl) {
    DIR* dir = opendir(dir_path);
    if (dir == NULL) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' ||
             (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (!should_skip_dir(entry->d_name)) {
                find_ve_files(path, fl);
            }
        } else if (S_ISREG(st.st_mode)) {
            if (has_ve_extension(entry->d_name)) {
                filelist_add(fl, path);
            }
        }
    }

    closedir(dir);
}

// ---- Format a single file ----

typedef enum {
    FMT_MODE_WRITE,
    FMT_MODE_CHECK,
    FMT_MODE_DIFF
} FmtMode;

static int format_file(const char* path, FmtMode mode) {
    size_t source_len = 0;
    char* source = read_file_contents(path, &source_len);
    if (source == NULL) return -1;

    size_t formatted_len = 0;
    char* formatted = fmt_format(source, source_len, &formatted_len);
    if (formatted == NULL) {
        free(source);
        fprintf(stderr, "Error: formatting failed for '%s'\n", path);
        return -1;
    }

    bool changed = (source_len != formatted_len ||
                    memcmp(source, formatted, source_len) != 0);

    int result = 0;

    if (changed) {
        switch (mode) {
            case FMT_MODE_WRITE:
                if (write_file_contents(path, formatted, formatted_len)) {
                    printf("%s\n", path);
                } else {
                    result = -1;
                }
                break;
            case FMT_MODE_CHECK:
                printf("%s\n", path);
                result = 1;
                break;
            case FMT_MODE_DIFF:
                print_diff(path, source, formatted);
                printf("\n");
                result = 1;
                break;
        }
    }

    free(source);
    free(formatted);
    return result;
}

// ---- Public entry point ----

int cmd_fmt_run(int argc, char** argv) {
    if (cli_has_flag(argc, argv, "--help")) {
        printf("Usage: vek fmt [options] [files...]\n");
        printf("\n");
        printf("Format .ve source files with opinionated style.\n");
        printf("\n");
        printf("Options:\n");
        printf("  --check    Check if files need formatting (exit 1 if any do)\n");
        printf("  --diff     Show what would change without writing\n");
        printf("  --help     Show this help message\n");
        printf("\n");
        printf("If no files are specified, recursively formats all .ve files\n");
        printf("in the current directory.\n");
        return 0;
    }

    bool check_mode = cli_has_flag(argc, argv, "--check");
    bool diff_mode = cli_has_flag(argc, argv, "--diff");

    FmtMode mode = FMT_MODE_WRITE;
    if (check_mode) mode = FMT_MODE_CHECK;
    else if (diff_mode) mode = FMT_MODE_DIFF;

    // Collect file arguments (non-flag arguments after "fmt")
    FileList files;
    filelist_init(&files);

    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) == 0) continue;
        filelist_add(&files, argv[i]);
    }

    // If no files specified, find all .ve files recursively
    if (files.count == 0) {
        find_ve_files(".", &files);
    }

    if (files.count == 0) {
        printf("No .ve files found.\n");
        filelist_free(&files);
        return 0;
    }

    int needs_formatting = 0;
    int errors = 0;

    for (size_t i = 0; i < files.count; i++) {
        int result = format_file(files.paths[i], mode);
        if (result > 0) needs_formatting++;
        if (result < 0) errors++;
    }

    filelist_free(&files);

    if (errors > 0) return 1;
    if (check_mode && needs_formatting > 0) return 1;
    return 0;
}
