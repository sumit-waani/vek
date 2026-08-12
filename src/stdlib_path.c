#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

// path.join(parts...) - joins URL path segments with '/'
static Value native_path_join(int arg_count, Value* args) {
    if (arg_count == 0) {
        return OBJ_VAL(obj_string_new("", 0));
    }

    // Calculate total length needed
    size_t total = 0;
    for (int i = 0; i < arg_count; i++) {
        if (!IS_STRING(args[i])) return VAL_NIL;
        ObjString* s = AS_STRING(args[i]);
        total += s->length + 1; // +1 for separator
    }

    char* buf = (char*)malloc(total + 1);
    if (!buf) return VAL_NIL;
    size_t pos = 0;

    for (int i = 0; i < arg_count; i++) {
        ObjString* s = AS_STRING(args[i]);
        const char* data = s->data;
        uint32_t len = s->length;

        // Skip leading slash if not first part, or if we already have a trailing slash
        uint32_t start = 0;
        if (i > 0 && len > 0 && data[0] == '/') {
            start = 1;
        }
        // Skip trailing slash
        uint32_t end = len;
        if (end > start && data[end - 1] == '/') {
            end--;
        }

        // Add separator before this part (unless this is the first part)
        if (i > 0 && pos > 0) {
            buf[pos++] = '/';
        }

        // Copy the part
        for (uint32_t j = start; j < end; j++) {
            buf[pos++] = data[j];
        }
    }

    // Ensure leading slash if the first part had one
    if (arg_count > 0) {
        ObjString* first = AS_STRING(args[0]);
        if (first->length > 0 && first->data[0] == '/') {
            // Result should start with /
            if (pos == 0 || buf[0] != '/') {
                // Shift everything right
                memmove(buf + 1, buf, pos);
                buf[0] = '/';
                pos++;
            }
        }
    }

    buf[pos] = '\0';
    ObjString* result = obj_string_new(buf, (uint32_t)pos);
    free(buf);
    return OBJ_VAL(result);
}

// path.normalize(p) - removes double slashes, trailing slash (except root)
static Value native_path_normalize(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ObjString* input = AS_STRING(args[0]);

    if (input->length == 0) {
        return OBJ_VAL(obj_string_new("", 0));
    }

    char* buf = (char*)malloc(input->length + 1);
    if (!buf) return VAL_NIL;
    size_t pos = 0;
    bool last_was_slash = false;

    for (uint32_t i = 0; i < input->length; i++) {
        char c = input->data[i];
        if (c == '/') {
            if (!last_was_slash) {
                buf[pos++] = '/';
            }
            last_was_slash = true;
        } else {
            buf[pos++] = c;
            last_was_slash = false;
        }
    }

    // Remove trailing slash (except for root "/")
    if (pos > 1 && buf[pos - 1] == '/') {
        pos--;
    }

    buf[pos] = '\0';
    ObjString* result = obj_string_new(buf, (uint32_t)pos);
    free(buf);
    return OBJ_VAL(result);
}

// path.basename(p) - returns last segment
static Value native_path_basename(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ObjString* input = AS_STRING(args[0]);

    if (input->length == 0) {
        return OBJ_VAL(obj_string_new("", 0));
    }

    // Find the last non-trailing-slash character
    int32_t end = (int32_t)input->length - 1;
    while (end > 0 && input->data[end] == '/') {
        end--;
    }

    // Find the last slash before end
    int32_t start = end;
    while (start > 0 && input->data[start - 1] != '/') {
        start--;
    }

    uint32_t len = (uint32_t)(end - start + 1);
    ObjString* result = obj_string_new(input->data + start, len);
    return OBJ_VAL(result);
}

// path.dirname(p) - returns parent path
static Value native_path_dirname(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ObjString* input = AS_STRING(args[0]);

    if (input->length == 0) {
        return OBJ_VAL(obj_string_new(".", 1));
    }

    // Find the last non-trailing-slash character
    int32_t end = (int32_t)input->length - 1;
    while (end > 0 && input->data[end] == '/') {
        end--;
    }

    // Find the last slash before end
    int32_t pos = end;
    while (pos > 0 && input->data[pos - 1] != '/') {
        pos--;
    }

    if (pos == 0) {
        // No slash found - parent is "."
        if (input->data[0] == '/') {
            return OBJ_VAL(obj_string_new("/", 1));
        }
        return OBJ_VAL(obj_string_new(".", 1));
    }

    // Remove trailing slash from dirname (except root)
    int32_t dir_end = pos - 1;
    if (dir_end == 0 && input->data[0] == '/') {
        dir_end = 1; // keep root "/"
    }

    ObjString* result = obj_string_new(input->data, (uint32_t)dir_end);
    return OBJ_VAL(result);
}

void stdlib_path_init(ObjMap* pkg) {
    stdlib_register(pkg, "join", native_path_join, -1);
    stdlib_register(pkg, "normalize", native_path_normalize, 1);
    stdlib_register(pkg, "basename", native_path_basename, 1);
    stdlib_register(pkg, "dirname", native_path_dirname, 1);
}
