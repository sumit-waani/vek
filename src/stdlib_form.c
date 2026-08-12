#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

#include <ctype.h>

// ---- Form state ----
// NOTE: These are process-global mutable statics. The current design assumes
// single-request-at-a-time processing. Concurrent or pipelined request
// handling would require per-request context objects instead.
static ObjMap* form_parsed = NULL;     // last parsed form data
static ObjList* form_error_list = NULL; // validation errors
static bool form_is_valid = true;

// ---- URL decoding ----

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// URL-decode a string in place. Returns decoded length.
static size_t url_decode(const char* src, size_t src_len, char* dst) {
    size_t j = 0;
    for (size_t i = 0; i < src_len; i++) {
        if (src[i] == '%' && i + 2 < src_len) {
            int hi = hex_digit(src[i+1]);
            int lo = hex_digit(src[i+2]);
            if (hi >= 0 && lo >= 0) {
                dst[j++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        if (src[i] == '+') {
            dst[j++] = ' ';
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
    return j;
}

// Parse URL-encoded form body into an ObjMap
static ObjMap* parse_form_body(const char* body, size_t body_len) {
    ObjMap* map = obj_map_new();
    gc_push_root(OBJ_VAL(map));

    const char* pos = body;
    const char* end = body + body_len;

    while (pos < end) {
        // Find next & or end
        const char* amp = pos;
        while (amp < end && *amp != '&') amp++;

        // Find = in this segment
        const char* eq = pos;
        while (eq < amp && *eq != '=') eq++;

        // Decode key
        size_t key_enc_len = (size_t)(eq - pos);
        char* key_buf = (char*)malloc(key_enc_len + 1);
        size_t key_len = url_decode(pos, key_enc_len, key_buf);

        ObjString* key = obj_string_new(key_buf, (uint32_t)key_len);
        free(key_buf);

        // Decode value
        Value value = OBJ_VAL(obj_string_new("", 0));
        if (eq < amp) {
            const char* val_start = eq + 1;
            size_t val_enc_len = (size_t)(amp - val_start);
            char* val_buf = (char*)malloc(val_enc_len + 1);
            size_t val_len = url_decode(val_start, val_enc_len, val_buf);
            value = OBJ_VAL(obj_string_new(val_buf, (uint32_t)val_len));
            free(val_buf);
        }

        obj_map_set(map, key, value);

        pos = amp + 1;
        if (amp >= end) break;
    }

    gc_pop_root();
    return map;
}

// ---- Validation helpers ----

static bool is_valid_email(const char* str, size_t len) {
    // Simple email validation: has @, has something before and after
    const char* at = NULL;
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '@') {
            at = str + i;
            break;
        }
    }
    if (!at) return false;
    if (at == str) return false; // nothing before @
    if ((size_t)(at - str) >= len - 1) return false; // nothing after @

    // Check there's a dot after @
    const char* dot = NULL;
    for (const char* p = at + 1; p < str + len; p++) {
        if (*p == '.') { dot = p; break; }
    }
    if (!dot) return false;
    if (dot == at + 1) return false; // nothing between @ and .
    if ((size_t)(dot - str) >= len - 1) return false; // nothing after .
    return true;
}

static bool is_valid_int(const char* str, size_t len) {
    if (len == 0) return false;
    size_t i = 0;
    if (str[0] == '-' || str[0] == '+') i++;
    if (i >= len) return false;
    for (; i < len; i++) {
        if (str[i] < '0' || str[i] > '9') return false;
    }
    return true;
}

static void add_error(const char* field, size_t field_len, const char* message) {
    ObjMap* err = obj_map_new();
    gc_push_root(OBJ_VAL(err));

    ObjString* field_key = obj_string_new("field", 5);
    ObjString* field_val = obj_string_new(field, (uint32_t)field_len);
    obj_map_set(err, field_key, OBJ_VAL(field_val));

    ObjString* msg_key = obj_string_new("message", 7);
    ObjString* msg_val = obj_string_new(message, (uint32_t)strlen(message));
    obj_map_set(err, msg_key, OBJ_VAL(msg_val));

    obj_list_push(form_error_list, OBJ_VAL(err));
    gc_pop_root();
}

// ---- Native functions ----

// form.parse(body_string) - parses URL-encoded form body
static Value native_form_parse(int argc, Value* args) {
    (void)argc;
    if (!IS_STRING(args[0])) return VAL_NIL;

    ObjString* body = AS_STRING(args[0]);
    ObjMap* parsed = parse_form_body(body->data, body->length);

    // Unpin old form_parsed before replacing to avoid pin leak
    if (form_parsed) {
        vm_unpin((ObjHeader*)form_parsed);
    }

    vm_pin((ObjHeader*)parsed);

    // Store as form_parsed
    form_parsed = parsed;

    return OBJ_VAL(parsed);
}

// form.validate(parsed_map, rules_map) - validates parsed form data against rules
// Rules map: {field_name: {required: true, min: 5, max: 100, type: "email"}}
static Value native_form_validate(int argc, Value* args) {
    (void)argc;
    if (!IS_MAP(args[0]) || !IS_MAP(args[1])) return VAL_NIL;

    ObjMap* data = AS_MAP(args[0]);
    ObjMap* rules = AS_MAP(args[1]);

    // Unpin old error list before replacing to avoid pin leak
    if (form_error_list) {
        vm_unpin((ObjHeader*)form_error_list);
    }

    // Reset errors
    form_error_list = obj_list_new();
    vm_pin((ObjHeader*)form_error_list);
    form_is_valid = true;

    // Unpin old form_parsed before replacing to avoid pin leak
    if (form_parsed) {
        vm_unpin((ObjHeader*)form_parsed);
    }

    // Store as form_parsed
    form_parsed = data;
    vm_pin((ObjHeader*)form_parsed);

    // Iterate through rules
    for (uint32_t i = 0; i < rules->capacity; i++) {
        if (rules->entries[i].key == NULL || rules->entries[i].key == MAP_TOMBSTONE) continue;

        ObjString* field_name = rules->entries[i].key;
        Value rule_val = rules->entries[i].value;

        if (!IS_MAP(rule_val)) continue;
        ObjMap* rule = AS_MAP(rule_val);

        // Get field value from data
        Value field_val;
        bool has_field = obj_map_get(data, field_name, &field_val);

        // Check 'required'
        ObjString* req_key = obj_string_new("required", 8);
        Value req_val;
        if (obj_map_get(rule, req_key, &req_val) && IS_BOOL(req_val) && AS_BOOL(req_val)) {
            if (!has_field || !IS_STRING(field_val) || AS_STRING(field_val)->length == 0) {
                add_error(field_name->data, field_name->length, "is required");
                form_is_valid = false;
                continue; // skip further checks for this field
            }
        }

        // If field is not present or empty, skip other validations
        if (!has_field || !IS_STRING(field_val) || AS_STRING(field_val)->length == 0) {
            continue;
        }

        ObjString* field_str = AS_STRING(field_val);

        // Check 'min' (minimum string length)
        ObjString* min_key = obj_string_new("min", 3);
        Value min_val;
        if (obj_map_get(rule, min_key, &min_val) && IS_INT(min_val)) {
            int64_t min_len = AS_INT(min_val);
            if ((int64_t)field_str->length < min_len) {
                char msg[128];
                snprintf(msg, sizeof(msg), "must be at least %lld characters", (long long)min_len);
                add_error(field_name->data, field_name->length, msg);
                form_is_valid = false;
            }
        }

        // Check 'max' (maximum string length)
        ObjString* max_key = obj_string_new("max", 3);
        Value max_val;
        if (obj_map_get(rule, max_key, &max_val) && IS_INT(max_val)) {
            int64_t max_len = AS_INT(max_val);
            if ((int64_t)field_str->length > max_len) {
                char msg[128];
                snprintf(msg, sizeof(msg), "must be at most %lld characters", (long long)max_len);
                add_error(field_name->data, field_name->length, msg);
                form_is_valid = false;
            }
        }

        // Check 'type'
        ObjString* type_key = obj_string_new("type", 4);
        Value type_val;
        if (obj_map_get(rule, type_key, &type_val) && IS_STRING(type_val)) {
            ObjString* type_str = AS_STRING(type_val);
            if (type_str->length == 5 && memcmp(type_str->data, "email", 5) == 0) {
                if (!is_valid_email(field_str->data, field_str->length)) {
                    add_error(field_name->data, field_name->length, "must be a valid email");
                    form_is_valid = false;
                }
            } else if (type_str->length == 3 && memcmp(type_str->data, "int", 3) == 0) {
                if (!is_valid_int(field_str->data, field_str->length)) {
                    add_error(field_name->data, field_name->length, "must be an integer");
                    form_is_valid = false;
                }
            }
        }
    }

    return BOOL_VAL(form_is_valid);
}

// form.valid() - returns bool (true if last validate had no errors)
static Value native_form_valid(int argc, Value* args) {
    (void)argc;
    (void)args;
    return BOOL_VAL(form_is_valid);
}

// form.errors() - returns list of maps [{field: "name", message: "is required"}, ...]
static Value native_form_errors(int argc, Value* args) {
    (void)argc;
    (void)args;
    if (!form_error_list) {
        form_error_list = obj_list_new();
        vm_pin((ObjHeader*)form_error_list);
    }
    return OBJ_VAL(form_error_list);
}

// form.get(field_name) - returns parsed value for field
static Value native_form_get(int argc, Value* args) {
    (void)argc;
    if (!IS_STRING(args[0])) return VAL_NIL;
    if (!form_parsed) return VAL_NIL;

    ObjString* key = AS_STRING(args[0]);
    Value val;
    if (obj_map_get(form_parsed, key, &val)) {
        return val;
    }
    return VAL_NIL;
}

void stdlib_form_init(ObjMap* pkg) {
    stdlib_register(pkg, "parse", native_form_parse, 1);
    stdlib_register(pkg, "validate", native_form_validate, 2);
    stdlib_register(pkg, "valid", native_form_valid, 0);
    stdlib_register(pkg, "errors", native_form_errors, 0);
    stdlib_register(pkg, "get", native_form_get, 1);
}
