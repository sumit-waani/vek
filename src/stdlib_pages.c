#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"
#include "compiler.h"

#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>

// Read an entire file into a heap-allocated string.
// Returns NULL on failure.
static char* pages_read_file(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }

    fseek(file, 0L, SEEK_END);
    size_t file_size = (size_t)ftell(file);
    rewind(file);

    char* buffer = (char*)malloc(file_size + 1);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }

    size_t bytes_read = fread(buffer, 1, file_size, file);
    if (bytes_read < file_size) {
        free(buffer);
        fclose(file);
        return NULL;
    }

    buffer[bytes_read] = '\0';
    fclose(file);
    return buffer;
}

// Convert a file path (relative to the pages/ directory) to a URL route path.
// Handles:
//   index.ve -> /
//   about.ve -> /about
//   posts/index.ve -> /posts
//   posts/[id].ve -> /posts/:id
// Caller must free the returned string.
static char* file_path_to_route(const char* rel_path) {
    // Strip the .ve extension
    size_t len = strlen(rel_path);
    if (len < 3) return NULL;

    // Copy without .ve extension
    size_t base_len = len - 3; // remove ".ve"
    char* base = (char*)malloc(base_len + 2); // +2 for leading / and NUL
    if (!base) return NULL;

    // Build route: prepend /
    base[0] = '/';
    memcpy(base + 1, rel_path, base_len);
    base[base_len + 1] = '\0';

    // Convert backslashes to forward slashes (for cross-platform safety)
    for (size_t i = 0; i < base_len + 1; i++) {
        if (base[i] == '\\') base[i] = '/';
    }

    // Handle index files: /index -> /, /posts/index -> /posts
    size_t route_len = strlen(base);
    if (route_len >= 6 && strcmp(base + route_len - 6, "/index") == 0) {
        if (route_len == 6) {
            // Just "/index" -> "/"
            base[1] = '\0';
        } else {
            // "/posts/index" -> "/posts"
            base[route_len - 6] = '\0';
        }
    }

    // Convert [param] to :param
    // First pass: calculate new length
    route_len = strlen(base);
    size_t new_len = 0;
    for (size_t i = 0; i < route_len; i++) {
        if (base[i] == '[') {
            new_len++; // for the ':'
            // Skip to ']'
            i++;
            while (i < route_len && base[i] != ']') {
                new_len++;
                i++;
            }
            // Don't count the ']'
        } else {
            new_len++;
        }
    }

    char* result = (char*)malloc(new_len + 1);
    if (!result) {
        free(base);
        return NULL;
    }

    size_t j = 0;
    for (size_t i = 0; i < route_len; i++) {
        if (base[i] == '[') {
            result[j++] = ':';
            i++;
            while (i < route_len && base[i] != ']') {
                result[j++] = base[i++];
            }
            // Skip the ']'
        } else {
            result[j++] = base[i];
        }
    }
    result[j] = '\0';

    free(base);
    return result;
}

// Recursively scan a directory for .ve files and execute them.
// base_dir is the root pages directory (for computing relative paths).
// current_dir is the directory currently being scanned.
// Returns the number of files successfully loaded, or -1 on error.
static int scan_directory(const char* base_dir, const char* current_dir) {
    DIR* dir = opendir(current_dir);
    if (!dir) {
        return -1;
    }

    int loaded = 0;
    struct dirent* entry;

    while ((entry = readdir(dir)) != NULL) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // Build full path
        size_t dir_len = strlen(current_dir);
        size_t name_len = strlen(entry->d_name);
        size_t full_len = dir_len + 1 + name_len;
        char* full_path = (char*)malloc(full_len + 1);
        if (!full_path) continue;
        snprintf(full_path, full_len + 1, "%s/%s", current_dir, entry->d_name);

        // Stat the entry to determine type
        struct stat st;
        if (stat(full_path, &st) != 0) {
            free(full_path);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            // Recursively scan subdirectory
            int sub_loaded = scan_directory(base_dir, full_path);
            if (sub_loaded > 0) {
                loaded += sub_loaded;
            }
        } else if (S_ISREG(st.st_mode)) {
            // Check if it ends with .ve
            if (name_len > 3 && strcmp(entry->d_name + name_len - 3, ".ve") == 0) {
                // Compute relative path from base_dir
                size_t base_len = strlen(base_dir);
                const char* rel_path = full_path + base_len;
                // Skip leading /
                if (rel_path[0] == '/') rel_path++;

                // Convert to route path (informational)
                char* route = file_path_to_route(rel_path);

                // Read and execute the file
                char* source = pages_read_file(full_path);
                if (source) {
                    // Compile the source into a function
                    ObjFunction* function = compile(source);
                    free(source);
                    if (function) {
                        vm_pin((ObjHeader*)function);
                        ObjClosure* closure = obj_closure_new(function);
                        vm_pin((ObjHeader*)closure);

                        // Push closure and call it via vm_call (re-entrant safe)
                        vm_push(OBJ_VAL(closure));
                        Value result_val = vm_call(OBJ_VAL(closure), 0);
                        (void)result_val;

                        vm_unpin((ObjHeader*)closure);
                        vm_unpin((ObjHeader*)function);
                        loaded++;
                    } else {
                        fprintf(stderr, "pages.scan: compile error in '%s' (route: %s)\n",
                                full_path, route ? route : "?");
                    }
                } else {
                    fprintf(stderr, "pages.scan: could not read '%s'\n", full_path);
                }

                if (route) free(route);
            }
        }

        free(full_path);
    }

    closedir(dir);
    return loaded;
}

// pages.scan(dir_path) - Recursively scan a directory for .ve page files
// and execute each one (which triggers their route registrations).
// Returns the number of page files loaded.
static Value native_pages_scan(int arg_count, Value* args) {
    (void)arg_count;

    if (!IS_STRING(args[0])) {
        fprintf(stderr, "pages.scan: expected string argument (directory path)\n");
        return INT_VAL(0);
    }

    ObjString* dir_str = AS_STRING(args[0]);
    // Make a null-terminated copy of the path
    char* dir_path = (char*)malloc(dir_str->length + 1);
    if (!dir_path) return INT_VAL(0);
    memcpy(dir_path, dir_str->data, dir_str->length);
    dir_path[dir_str->length] = '\0';

    int loaded = scan_directory(dir_path, dir_path);
    free(dir_path);

    if (loaded < 0) {
        return INT_VAL(0);
    }
    return INT_VAL(loaded);
}

// pages.route(file_path) - Convert a file path to a URL route path.
// Useful for debugging/introspection.
// e.g., pages.route("posts/[id].ve") -> "/posts/:id"
static Value native_pages_route(int arg_count, Value* args) {
    (void)arg_count;

    if (!IS_STRING(args[0])) {
        return VAL_NIL;
    }

    ObjString* path_str = AS_STRING(args[0]);
    char* path_copy = (char*)malloc(path_str->length + 1);
    if (!path_copy) return VAL_NIL;
    memcpy(path_copy, path_str->data, path_str->length);
    path_copy[path_str->length] = '\0';

    char* route = file_path_to_route(path_copy);
    free(path_copy);

    if (!route) return VAL_NIL;

    ObjString* result = obj_string_new(route, (uint32_t)strlen(route));
    free(route);
    return OBJ_VAL(result);
}

void stdlib_pages_init(ObjMap* pkg) {
    stdlib_register(pkg, "scan", native_pages_scan, 1);
    stdlib_register(pkg, "route", native_pages_route, 1);
}
