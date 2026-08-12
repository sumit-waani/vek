#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

// ---- Dynamic buffer helpers ----

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} SanBuf;

static void san_buf_init(SanBuf* b) {
    b->cap = 256;
    b->data = (char*)malloc(b->cap);
    b->len = 0;
}

static void san_buf_ensure(SanBuf* b, size_t extra) {
    while (b->len + extra >= b->cap) {
        b->cap *= 2;
        b->data = (char*)realloc(b->data, b->cap);
    }
}

static void san_buf_append(SanBuf* b, const char* s, size_t n) {
    san_buf_ensure(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
}

static void san_buf_char(SanBuf* b, char c) {
    san_buf_ensure(b, 1);
    b->data[b->len++] = c;
}

// ---- Helper: case-insensitive compare for tag names ----

static bool tag_name_eq(const char* a, size_t alen, const char* b, size_t blen) {
    if (alen != blen) return false;
    for (size_t i = 0; i < alen; i++) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
    }
    return true;
}

// ---- Check if tag is in allowed list ----

static bool is_tag_allowed(const char* tag_name, size_t tag_len, ObjList* allowed_tags) {
    if (!allowed_tags) return false;
    for (uint32_t i = 0; i < allowed_tags->length; i++) {
        Value v = obj_list_get(allowed_tags, i);
        if (IS_STRING(v)) {
            ObjString* s = AS_STRING(v);
            if (tag_name_eq(tag_name, tag_len, s->data, s->length)) {
                return true;
            }
        }
    }
    return false;
}

// ---- Check if an attribute is allowed for a given tag ----

static bool is_attr_allowed(const char* tag_name, size_t tag_len,
                            const char* attr_name, size_t attr_len,
                            ObjMap* attrs_config) {
    if (!attrs_config) return false;

    // Look for the tag in the attrs config map
    for (uint32_t i = 0; i < attrs_config->capacity; i++) {
        if (attrs_config->entries[i].key == NULL ||
            attrs_config->entries[i].key == MAP_TOMBSTONE) continue;

        ObjString* key = attrs_config->entries[i].key;
        if (tag_name_eq(tag_name, tag_len, key->data, key->length)) {
            Value val = attrs_config->entries[i].value;
            if (IS_LIST(val)) {
                ObjList* attr_list = AS_LIST(val);
                for (uint32_t j = 0; j < attr_list->length; j++) {
                    Value av = obj_list_get(attr_list, j);
                    if (IS_STRING(av)) {
                        ObjString* as = AS_STRING(av);
                        if (tag_name_eq(attr_name, attr_len, as->data, as->length)) {
                            return true;
                        }
                    }
                }
            }
            return false;
        }
    }
    return false;
}

// ---- sanitize.html(string, config_map) ----
// Config map: { tags: ["b", "i", "a", ...], attrs: { "a": ["href"] } }

static Value native_sanitize_html(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;
    if (!IS_MAP(args[1])) return VAL_NIL;

    ObjString* input = AS_STRING(args[0]);
    ObjMap* config = AS_MAP(args[1]);

    // Extract tags list
    ObjList* allowed_tags = NULL;
    ObjString* tags_key = obj_string_new("tags", 4);
    Value tags_val;
    if (obj_map_get(config, tags_key, &tags_val) && IS_LIST(tags_val)) {
        allowed_tags = AS_LIST(tags_val);
    }

    // Extract attrs map
    ObjMap* attrs_config = NULL;
    ObjString* attrs_key = obj_string_new("attrs", 5);
    Value attrs_val;
    if (obj_map_get(config, attrs_key, &attrs_val) && IS_MAP(attrs_val)) {
        attrs_config = AS_MAP(attrs_val);
    }

    SanBuf buf;
    san_buf_init(&buf);

    const char* s = input->data;
    size_t len = input->length;
    size_t i = 0;

    while (i < len) {
        if (s[i] == '<') {
            size_t tag_start = i;
            i++;

            // Check for closing tag
            bool is_closing = false;
            if (i < len && s[i] == '/') {
                is_closing = true;
                i++;
            }

            // Parse tag name
            size_t name_start = i;
            while (i < len && s[i] != ' ' && s[i] != '>' && s[i] != '/' && s[i] != '\n') {
                i++;
            }
            size_t name_len = i - name_start;

            if (name_len == 0) {
                // Not a real tag, output literal <
                san_buf_append(&buf, "&lt;", 4);
                i = tag_start + 1;
                continue;
            }

            // Check if tag is allowed
            bool allowed = is_tag_allowed(s + name_start, name_len, allowed_tags);

            if (!allowed) {
                // Skip the entire tag
                while (i < len && s[i] != '>') i++;
                if (i < len) i++; // skip >
                continue;
            }

            // Tag is allowed - rebuild it with only allowed attributes
            san_buf_char(&buf, '<');
            if (is_closing) san_buf_char(&buf, '/');
            san_buf_append(&buf, s + name_start, name_len);

            // Parse and filter attributes
            if (!is_closing) {
                while (i < len && s[i] != '>' && s[i] != '/') {
                    // Skip whitespace
                    while (i < len && s[i] == ' ') i++;
                    if (i >= len || s[i] == '>' || s[i] == '/') break;

                    // Parse attribute name
                    size_t attr_start = i;
                    while (i < len && s[i] != '=' && s[i] != ' ' && s[i] != '>' && s[i] != '/') {
                        i++;
                    }
                    size_t attr_len = i - attr_start;

                    // Parse attribute value (if present)
                    char* attr_value = NULL;
                    size_t attr_value_len = 0;
                    if (i < len && s[i] == '=') {
                        i++; // skip =
                        if (i < len && (s[i] == '"' || s[i] == '\'')) {
                            char quote = s[i];
                            i++; // skip opening quote
                            size_t val_start = i;
                            while (i < len && s[i] != quote) i++;
                            attr_value = (char*)(s + val_start);
                            attr_value_len = i - val_start;
                            if (i < len) i++; // skip closing quote
                        } else {
                            // unquoted value
                            size_t val_start = i;
                            while (i < len && s[i] != ' ' && s[i] != '>') i++;
                            attr_value = (char*)(s + val_start);
                            attr_value_len = i - val_start;
                        }
                    }

                    // Check if attribute is allowed
                    if (attr_len > 0 && is_attr_allowed(s + name_start, name_len,
                                                         s + attr_start, attr_len,
                                                         attrs_config)) {
                        san_buf_char(&buf, ' ');
                        san_buf_append(&buf, s + attr_start, attr_len);
                        if (attr_value) {
                            san_buf_append(&buf, "=\"", 2);
                            san_buf_append(&buf, attr_value, attr_value_len);
                            san_buf_char(&buf, '"');
                        }
                    }
                }
            }

            // Skip to end of tag
            while (i < len && s[i] != '>') i++;
            if (i < len) i++; // skip >

            san_buf_char(&buf, '>');
        } else {
            san_buf_char(&buf, s[i]);
            i++;
        }
    }

    ObjString* result = obj_string_new(buf.data, (uint32_t)buf.len);
    free(buf.data);
    return OBJ_VAL(result);
}

// ---- sanitize.strip(string) ----
// Removes ALL HTML tags, returns plain text

static Value native_sanitize_strip(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ObjString* input = AS_STRING(args[0]);

    SanBuf buf;
    san_buf_init(&buf);

    const char* s = input->data;
    size_t len = input->length;
    size_t i = 0;

    while (i < len) {
        if (s[i] == '<') {
            // Skip until closing >
            while (i < len && s[i] != '>') i++;
            if (i < len) i++; // skip >
        } else {
            san_buf_char(&buf, s[i]);
            i++;
        }
    }

    ObjString* result = obj_string_new(buf.data, (uint32_t)buf.len);
    free(buf.data);
    return OBJ_VAL(result);
}

void stdlib_sanitize_init(ObjMap* pkg) {
    stdlib_register(pkg, "html", native_sanitize_html, 2);
    stdlib_register(pkg, "strip", native_sanitize_strip, 1);
}
