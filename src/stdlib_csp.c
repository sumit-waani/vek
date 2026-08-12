#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

#include <fcntl.h>
#include <unistd.h>

// Map from underscore-style keys to CSP directive names
static const char* csp_directive_name(const char* key, uint32_t len) {
    // Map common keys: default_src -> default-src, etc.
    if (len == 11 && memcmp(key, "default_src", 11) == 0) return "default-src";
    if (len == 10 && memcmp(key, "script_src", 10) == 0) return "script-src";
    if (len == 9  && memcmp(key, "style_src", 9) == 0) return "style-src";
    if (len == 7  && memcmp(key, "img_src", 7) == 0) return "img-src";
    if (len == 8  && memcmp(key, "font_src", 8) == 0) return "font-src";
    if (len == 11 && memcmp(key, "connect_src", 11) == 0) return "connect-src";
    if (len == 9  && memcmp(key, "media_src", 9) == 0) return "media-src";
    if (len == 10 && memcmp(key, "object_src", 10) == 0) return "object-src";
    if (len == 9  && memcmp(key, "frame_src", 9) == 0) return "frame-src";
    if (len == 9  && memcmp(key, "child_src", 9) == 0) return "child-src";
    if (len == 10 && memcmp(key, "worker_src", 10) == 0) return "worker-src";
    if (len == 15 && memcmp(key, "frame_ancestors", 15) == 0) return "frame-ancestors";
    if (len == 8  && memcmp(key, "base_uri", 8) == 0) return "base-uri";
    if (len == 11 && memcmp(key, "form_action", 11) == 0) return "form-action";
    if (len == 10 && memcmp(key, "report_uri", 10) == 0) return "report-uri";
    // If already has hyphen or is unrecognized, use as-is (we'll copy it)
    return NULL;
}

// csp.build(map) - build Content-Security-Policy header string from directive map
static Value native_csp_build(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_MAP(args[0])) return VAL_NIL;
    ObjMap* directives = AS_MAP(args[0]);

    // Build the CSP string
    char* buf = NULL;
    size_t len = 0;
    size_t cap = 0;

    bool first = true;
    for (uint32_t i = 0; i < directives->capacity; i++) {
        if (directives->entries[i].key == NULL || directives->entries[i].key == MAP_TOMBSTONE) continue;

        ObjString* key = directives->entries[i].key;
        Value val = directives->entries[i].value;
        if (!IS_STRING(val)) continue;
        ObjString* value = AS_STRING(val);

        // Add separator
        if (!first) {
            // Ensure space in buffer
            while (len + 2 > cap) { cap = cap < 64 ? 64 : cap * 2; buf = (char*)realloc(buf, cap); }
            buf[len++] = ';';
            buf[len++] = ' ';
        }
        first = false;

        // Get directive name
        const char* directive = csp_directive_name(key->data, key->length);
        const char* name_to_use;
        size_t name_len;
        if (directive) {
            name_to_use = directive;
            name_len = strlen(directive);
        } else {
            name_to_use = key->data;
            name_len = key->length;
        }

        // Append "directive value"
        size_t needed = name_len + 1 + value->length;
        while (len + needed + 1 > cap) { cap = cap < 64 ? 64 : cap * 2; buf = (char*)realloc(buf, cap); }
        memcpy(buf + len, name_to_use, name_len);
        len += name_len;
        buf[len++] = ' ';
        memcpy(buf + len, value->data, value->length);
        len += value->length;
    }

    if (!buf) {
        return OBJ_VAL(obj_string_new("", 0));
    }

    buf[len] = '\0';
    ObjString* result = obj_string_new(buf, (uint32_t)len);
    free(buf);
    return OBJ_VAL(result);
}

// csp.nonce() - returns a random base64 nonce (16 bytes of randomness)
static Value native_csp_nonce(int arg_count, Value* args) {
    (void)arg_count;
    (void)args;

    uint8_t random_bytes[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return VAL_NIL;

    size_t total = 0;
    while (total < 16) {
        ssize_t r = read(fd, random_bytes + total, 16 - total);
        if (r <= 0) { close(fd); return VAL_NIL; }
        total += (size_t)r;
    }
    close(fd);

    // Base64 encode
    static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t olen = 4 * ((16 + 2) / 3); // 24 chars
    char out[25];
    size_t i = 0, j = 0;
    while (i < 16) {
        uint32_t a = (i < 16) ? random_bytes[i++] : 0;
        uint32_t b = (i < 16) ? random_bytes[i++] : 0;
        uint32_t c = (i < 16) ? random_bytes[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;
        out[j++] = b64_table[(triple >> 18) & 0x3F];
        out[j++] = b64_table[(triple >> 12) & 0x3F];
        out[j++] = b64_table[(triple >> 6) & 0x3F];
        out[j++] = b64_table[triple & 0x3F];
    }
    // 16 bytes -> no padding needed (16 % 3 == 1, so 2 padding chars)
    size_t mod = 16 % 3;
    if (mod == 1) { out[olen - 1] = '='; out[olen - 2] = '='; }
    else if (mod == 2) { out[olen - 1] = '='; }
    out[olen] = '\0';

    return OBJ_VAL(obj_string_new(out, (uint32_t)olen));
}

void stdlib_csp_init(ObjMap* pkg) {
    stdlib_register(pkg, "build", native_csp_build, 1);
    stdlib_register(pkg, "nonce", native_csp_nonce, 0);
}
