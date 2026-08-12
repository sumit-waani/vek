#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

#include <math.h>
#include <ctype.h>

// ---- JSON Encoder ----

// Forward declaration
static void json_encode_value(Value value, char** buf, size_t* len, size_t* cap);

static void json_buf_append(char** buf, size_t* len, size_t* cap, const char* str, size_t slen) {
    while (*len + slen + 1 > *cap) {
        *cap = (*cap < 64) ? 64 : *cap * 2;
        *buf = (char*)realloc(*buf, *cap);
    }
    memcpy(*buf + *len, str, slen);
    *len += slen;
    (*buf)[*len] = '\0';
}

static void json_buf_char(char** buf, size_t* len, size_t* cap, char c) {
    json_buf_append(buf, len, cap, &c, 1);
}

static void json_encode_string(ObjString* str, char** buf, size_t* len, size_t* cap) {
    json_buf_char(buf, len, cap, '"');
    for (uint32_t i = 0; i < str->length; i++) {
        char c = str->data[i];
        switch (c) {
            case '"':  json_buf_append(buf, len, cap, "\\\"", 2); break;
            case '\\': json_buf_append(buf, len, cap, "\\\\", 2); break;
            case '\n': json_buf_append(buf, len, cap, "\\n", 2); break;
            case '\r': json_buf_append(buf, len, cap, "\\r", 2); break;
            case '\t': json_buf_append(buf, len, cap, "\\t", 2); break;
            default:
                if ((unsigned char)c < 0x20) {
                    char esc[8];
                    int elen = snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)c);
                    json_buf_append(buf, len, cap, esc, (size_t)elen);
                } else {
                    json_buf_char(buf, len, cap, c);
                }
                break;
        }
    }
    json_buf_char(buf, len, cap, '"');
}

static void json_encode_value(Value value, char** buf, size_t* len, size_t* cap) {
    if (IS_NIL(value)) {
        json_buf_append(buf, len, cap, "null", 4);
    } else if (IS_BOOL(value)) {
        if (AS_BOOL(value)) {
            json_buf_append(buf, len, cap, "true", 4);
        } else {
            json_buf_append(buf, len, cap, "false", 5);
        }
    } else if (IS_INT(value)) {
        char num[32];
        int nlen = snprintf(num, sizeof(num), "%lld", (long long)AS_INT(value));
        json_buf_append(buf, len, cap, num, (size_t)nlen);
    } else if (IS_FLOAT(value)) {
        double d = AS_DOUBLE(value);
        if (isnan(d) || isinf(d)) {
            json_buf_append(buf, len, cap, "null", 4);
        } else {
            char num[64];
            int nlen = snprintf(num, sizeof(num), "%g", d);
            json_buf_append(buf, len, cap, num, (size_t)nlen);
        }
    } else if (IS_STRING(value)) {
        json_encode_string(AS_STRING(value), buf, len, cap);
    } else if (IS_LIST(value)) {
        ObjList* list = AS_LIST(value);
        json_buf_char(buf, len, cap, '[');
        for (uint32_t i = 0; i < list->length; i++) {
            if (i > 0) json_buf_char(buf, len, cap, ',');
            json_encode_value(list->data[i], buf, len, cap);
        }
        json_buf_char(buf, len, cap, ']');
    } else if (IS_MAP(value)) {
        ObjMap* map = AS_MAP(value);
        json_buf_char(buf, len, cap, '{');
        bool first = true;
        for (uint32_t i = 0; i < map->capacity; i++) {
            if (map->entries[i].key == NULL) continue;
            // Skip tombstones - compare with extern MAP_TOMBSTONE
            if (map->entries[i].key == MAP_TOMBSTONE) continue;
            if (!first) json_buf_char(buf, len, cap, ',');
            first = false;
            json_encode_string(map->entries[i].key, buf, len, cap);
            json_buf_char(buf, len, cap, ':');
            json_encode_value(map->entries[i].value, buf, len, cap);
        }
        json_buf_char(buf, len, cap, '}');
    } else {
        json_buf_append(buf, len, cap, "null", 4);
    }
}

static Value native_json_encode(int arg_count, Value* args) {
    (void)arg_count;
    char* buf = NULL;
    size_t len = 0, cap = 0;
    json_encode_value(args[0], &buf, &len, &cap);
    ObjString* result = obj_string_new(buf ? buf : "", (uint32_t)len);
    free(buf);
    return OBJ_VAL(result);
}

// ---- JSON Decoder ----

typedef struct {
    const char* src;
    size_t pos;
    size_t len;
} JsonParser;

static void json_skip_ws(JsonParser* p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            p->pos++;
        } else {
            break;
        }
    }
}

static char json_peek(JsonParser* p) {
    json_skip_ws(p);
    if (p->pos >= p->len) return '\0';
    return p->src[p->pos];
}

static char json_advance(JsonParser* p) {
    if (p->pos >= p->len) return '\0';
    return p->src[p->pos++];
}

static bool json_match(JsonParser* p, const char* str, size_t slen) {
    json_skip_ws(p);
    if (p->pos + slen > p->len) return false;
    if (memcmp(p->src + p->pos, str, slen) == 0) {
        p->pos += slen;
        return true;
    }
    return false;
}

// Forward declaration
static Value json_parse_value(JsonParser* p, bool* ok);

static Value json_parse_string(JsonParser* p, bool* ok) {
    json_skip_ws(p);
    if (json_advance(p) != '"') { *ok = false; return VAL_NIL; }

    char* buf = NULL;
    size_t len = 0, cap = 0;

    while (p->pos < p->len) {
        char c = p->src[p->pos++];
        if (c == '"') {
            ObjString* s = obj_string_new(buf ? buf : "", (uint32_t)len);
            free(buf);
            return OBJ_VAL(s);
        }
        if (c == '\\') {
            if (p->pos >= p->len) { *ok = false; free(buf); return VAL_NIL; }
            char esc = p->src[p->pos++];
            switch (esc) {
                case '"':  c = '"'; break;
                case '\\': c = '\\'; break;
                case '/':  c = '/'; break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                case 'u': {
                    // Parse 4 hex digits (simplified - just skip for now)
                    if (p->pos + 4 > p->len) { *ok = false; free(buf); return VAL_NIL; }
                    unsigned int cp = 0;
                    for (int i = 0; i < 4; i++) {
                        char h = p->src[p->pos++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                        else { *ok = false; free(buf); return VAL_NIL; }
                    }
                    // Encode as UTF-8
                    if (cp < 0x80) {
                        c = (char)cp;
                    } else if (cp < 0x800) {
                        json_buf_char(&buf, &len, &cap, (char)(0xC0 | (cp >> 6)));
                        c = (char)(0x80 | (cp & 0x3F));
                    } else {
                        json_buf_char(&buf, &len, &cap, (char)(0xE0 | (cp >> 12)));
                        json_buf_char(&buf, &len, &cap, (char)(0x80 | ((cp >> 6) & 0x3F)));
                        c = (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: c = esc; break;
            }
        }
        json_buf_char(&buf, &len, &cap, c);
    }

    *ok = false;
    free(buf);
    return VAL_NIL;
}

static Value json_parse_number(JsonParser* p, bool* ok) {
    json_skip_ws(p);
    size_t start = p->pos;
    bool is_float = false;

    if (p->pos < p->len && p->src[p->pos] == '-') p->pos++;
    while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    if (p->pos < p->len && p->src[p->pos] == '.') {
        is_float = true;
        p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    }
    if (p->pos < p->len && (p->src[p->pos] == 'e' || p->src[p->pos] == 'E')) {
        is_float = true;
        p->pos++;
        if (p->pos < p->len && (p->src[p->pos] == '+' || p->src[p->pos] == '-')) p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    }

    if (p->pos == start) { *ok = false; return VAL_NIL; }

    char tmp[64];
    size_t nlen = p->pos - start;
    if (nlen >= sizeof(tmp)) nlen = sizeof(tmp) - 1;
    memcpy(tmp, p->src + start, nlen);
    tmp[nlen] = '\0';

    if (is_float) {
        return FLOAT_VAL(strtod(tmp, NULL));
    } else {
        return INT_VAL((int64_t)strtoll(tmp, NULL, 10));
    }
}

static Value json_parse_array(JsonParser* p, bool* ok) {
    json_skip_ws(p);
    p->pos++; // skip '['

    ObjList* list = obj_list_new();
    gc_push_root(OBJ_VAL(list));

    json_skip_ws(p);
    if (json_peek(p) == ']') {
        p->pos++;
        gc_pop_root();
        return OBJ_VAL(list);
    }

    for (;;) {
        Value val = json_parse_value(p, ok);
        if (!*ok) { gc_pop_root(); return VAL_NIL; }
        obj_list_push(list, val);

        json_skip_ws(p);
        if (p->pos >= p->len) { *ok = false; gc_pop_root(); return VAL_NIL; }
        char c = p->src[p->pos];
        if (c == ']') { p->pos++; break; }
        if (c == ',') { p->pos++; continue; }
        *ok = false; gc_pop_root(); return VAL_NIL;
    }

    gc_pop_root();
    return OBJ_VAL(list);
}

static Value json_parse_object(JsonParser* p, bool* ok) {
    json_skip_ws(p);
    p->pos++; // skip '{'

    ObjMap* map = obj_map_new();
    gc_push_root(OBJ_VAL(map));

    json_skip_ws(p);
    if (json_peek(p) == '}') {
        p->pos++;
        gc_pop_root();
        return OBJ_VAL(map);
    }

    for (;;) {
        json_skip_ws(p);
        if (json_peek(p) != '"') { *ok = false; gc_pop_root(); return VAL_NIL; }
        Value key_val = json_parse_string(p, ok);
        if (!*ok) { gc_pop_root(); return VAL_NIL; }

        json_skip_ws(p);
        if (p->pos >= p->len || p->src[p->pos] != ':') { *ok = false; gc_pop_root(); return VAL_NIL; }
        p->pos++; // skip ':'

        Value val = json_parse_value(p, ok);
        if (!*ok) { gc_pop_root(); return VAL_NIL; }

        obj_map_set(map, AS_STRING(key_val), val);

        json_skip_ws(p);
        if (p->pos >= p->len) { *ok = false; gc_pop_root(); return VAL_NIL; }
        char c = p->src[p->pos];
        if (c == '}') { p->pos++; break; }
        if (c == ',') { p->pos++; continue; }
        *ok = false; gc_pop_root(); return VAL_NIL;
    }

    gc_pop_root();
    return OBJ_VAL(map);
}

static Value json_parse_value(JsonParser* p, bool* ok) {
    json_skip_ws(p);
    if (p->pos >= p->len) { *ok = false; return VAL_NIL; }

    char c = p->src[p->pos];
    switch (c) {
        case '"': return json_parse_string(p, ok);
        case '{': return json_parse_object(p, ok);
        case '[': return json_parse_array(p, ok);
        case 't':
            if (json_match(p, "true", 4)) return VAL_TRUE;
            *ok = false; return VAL_NIL;
        case 'f':
            if (json_match(p, "false", 5)) return VAL_FALSE;
            *ok = false; return VAL_NIL;
        case 'n':
            if (json_match(p, "null", 4)) return VAL_NIL;
            *ok = false; return VAL_NIL;
        default:
            if (c == '-' || isdigit((unsigned char)c)) {
                return json_parse_number(p, ok);
            }
            *ok = false;
            return VAL_NIL;
    }
}

static Value native_json_decode(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) {
        return VAL_NIL;
    }
    ObjString* input = AS_STRING(args[0]);
    JsonParser parser = { .src = input->data, .pos = 0, .len = input->length };
    bool ok = true;
    Value result = json_parse_value(&parser, &ok);
    if (!ok) return VAL_NIL;
    return result;
}

void stdlib_json_init(ObjMap* pkg) {
    stdlib_register(pkg, "encode", native_json_encode, 1);
    stdlib_register(pkg, "decode", native_json_decode, 1);
}
