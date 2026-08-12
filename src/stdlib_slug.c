#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

// slug.make(string) - convert string to URL slug
// - lowercase
// - replace non-alphanumeric with hyphens
// - collapse multiple hyphens
// - trim leading/trailing hyphens
static Value native_slug_make(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ObjString* input = AS_STRING(args[0]);

    if (input->length == 0) {
        return OBJ_VAL(obj_string_new("", 0));
    }

    // Allocate worst-case buffer (same length as input)
    char* buf = (char*)malloc(input->length + 1);
    if (!buf) return VAL_NIL;

    uint32_t out_len = 0;
    bool last_was_hyphen = true; // treat start as hyphen to trim leading

    for (uint32_t i = 0; i < input->length; i++) {
        char c = input->data[i];

        // Lowercase
        if (c >= 'A' && c <= 'Z') {
            c = c + ('a' - 'A');
        }

        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            buf[out_len++] = c;
            last_was_hyphen = false;
        } else {
            // Replace non-alphanumeric with hyphen, collapsing multiples
            if (!last_was_hyphen) {
                buf[out_len++] = '-';
                last_was_hyphen = true;
            }
        }
    }

    // Trim trailing hyphen
    if (out_len > 0 && buf[out_len - 1] == '-') {
        out_len--;
    }

    ObjString* result = obj_string_new(buf, out_len);
    free(buf);
    return OBJ_VAL(result);
}

void stdlib_slug_init(ObjMap* pkg) {
    stdlib_register(pkg, "make", native_slug_make, 1);
}
