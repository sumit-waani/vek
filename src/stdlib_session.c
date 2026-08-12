#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

#include <fcntl.h>
#include <unistd.h>

// ---- Session state ----
static ObjMap* session_data = NULL;     // current session key-value store
static ObjMap* flash_data = NULL;       // flash messages (one-time read)
static char* session_secret = NULL;     // HMAC signing key
static size_t session_secret_len = 0;

// ---- SHA-256 implementation (duplicated from crypto for internal use) ----

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define SHA256_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define SHA256_CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define SHA256_MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SHA256_EP0(x) (SHA256_ROTR(x, 2) ^ SHA256_ROTR(x, 13) ^ SHA256_ROTR(x, 22))
#define SHA256_EP1(x) (SHA256_ROTR(x, 6) ^ SHA256_ROTR(x, 11) ^ SHA256_ROTR(x, 25))
#define SHA256_SIG0(x) (SHA256_ROTR(x, 7) ^ SHA256_ROTR(x, 18) ^ ((x) >> 3))
#define SHA256_SIG1(x) (SHA256_ROTR(x, 17) ^ SHA256_ROTR(x, 19) ^ ((x) >> 10))

static void session_sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    uint32_t h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    size_t padded_len = ((len + 8) / 64 + 1) * 64;
    uint8_t* msg = (uint8_t*)calloc(padded_len, 1);
    memcpy(msg, data, len);
    msg[len] = 0x80;
    uint64_t bit_len = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) {
        msg[padded_len - 1 - i] = (uint8_t)(bit_len >> (i * 8));
    }

    for (size_t offset = 0; offset < padded_len; offset += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)msg[offset + i*4] << 24) |
                    ((uint32_t)msg[offset + i*4+1] << 16) |
                    ((uint32_t)msg[offset + i*4+2] << 8) |
                    ((uint32_t)msg[offset + i*4+3]);
        }
        for (int i = 16; i < 64; i++) {
            w[i] = SHA256_SIG1(w[i-2]) + w[i-7] + SHA256_SIG0(w[i-15]) + w[i-16];
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + SHA256_EP1(e) + SHA256_CH(e, f, g) + sha256_k[i] + w[i];
            uint32_t t2 = SHA256_EP0(a) + SHA256_MAJ(a, b, c);
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }

        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    free(msg);

    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(h[i] >> 24);
        out[i*4+1] = (uint8_t)(h[i] >> 16);
        out[i*4+2] = (uint8_t)(h[i] >> 8);
        out[i*4+3] = (uint8_t)(h[i]);
    }
}

// ---- HMAC-SHA256 ----

void hmac_sha256(const uint8_t* key, size_t key_len,
                 const uint8_t* msg, size_t msg_len,
                 uint8_t out[32]) {
    uint8_t k[64];
    memset(k, 0, 64);

    // If key > 64 bytes, hash it first
    if (key_len > 64) {
        session_sha256(key, key_len, k);
        // k is now 32 bytes of hash + 32 bytes of zeros
    } else {
        memcpy(k, key, key_len);
    }

    // ipad = key XOR 0x36
    uint8_t ipad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
    }

    // opad = key XOR 0x5c
    uint8_t opad[64];
    for (int i = 0; i < 64; i++) {
        opad[i] = k[i] ^ 0x5c;
    }

    // inner = SHA256(ipad || message)
    size_t inner_len = 64 + msg_len;
    uint8_t* inner_data = (uint8_t*)malloc(inner_len);
    memcpy(inner_data, ipad, 64);
    memcpy(inner_data + 64, msg, msg_len);

    uint8_t inner_hash[32];
    session_sha256(inner_data, inner_len, inner_hash);
    free(inner_data);

    // outer = SHA256(opad || inner_hash)
    uint8_t outer_data[96]; // 64 + 32
    memcpy(outer_data, opad, 64);
    memcpy(outer_data + 64, inner_hash, 32);

    session_sha256(outer_data, 96, out);
}

// ---- Base64 encode/decode ----

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char* base64_encode(const uint8_t* data, size_t len, size_t* out_len) {
    size_t olen = 4 * ((len + 2) / 3);
    char* out = (char*)malloc(olen + 1);
    if (!out) return NULL;

    size_t i = 0, j = 0;
    while (i < len) {
        uint32_t a = (i < len) ? data[i++] : 0;
        uint32_t b = (i < len) ? data[i++] : 0;
        uint32_t c = (i < len) ? data[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;

        out[j++] = b64_table[(triple >> 18) & 0x3F];
        out[j++] = b64_table[(triple >> 12) & 0x3F];
        out[j++] = b64_table[(triple >> 6) & 0x3F];
        out[j++] = b64_table[triple & 0x3F];
    }

    // Padding
    size_t mod = len % 3;
    if (mod == 1) {
        out[olen - 1] = '=';
        out[olen - 2] = '=';
    } else if (mod == 2) {
        out[olen - 1] = '=';
    }

    out[olen] = '\0';
    if (out_len) *out_len = olen;
    return out;
}

static int b64_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

uint8_t* base64_decode(const char* data, size_t len, size_t* out_len) {
    if (len % 4 != 0) return NULL;

    size_t olen = (len / 4) * 3;
    if (len > 0 && data[len - 1] == '=') olen--;
    if (len > 1 && data[len - 2] == '=') olen--;

    uint8_t* out = (uint8_t*)malloc(olen + 1);
    if (!out) return NULL;

    size_t i = 0, j = 0;
    while (i < len) {
        int a = (data[i] == '=') ? 0 : b64_decode_char(data[i]); i++;
        int b = (data[i] == '=') ? 0 : b64_decode_char(data[i]); i++;
        int c = (data[i] == '=') ? 0 : b64_decode_char(data[i]); i++;
        int d = (data[i] == '=') ? 0 : b64_decode_char(data[i]); i++;

        if (a < 0 || b < 0 || c < 0 || d < 0) {
            free(out);
            return NULL;
        }

        uint32_t triple = ((uint32_t)a << 18) | ((uint32_t)b << 12) |
                          ((uint32_t)c << 6) | (uint32_t)d;

        if (j < olen) out[j++] = (uint8_t)((triple >> 16) & 0xFF);
        if (j < olen) out[j++] = (uint8_t)((triple >> 8) & 0xFF);
        if (j < olen) out[j++] = (uint8_t)(triple & 0xFF);
    }

    out[olen] = '\0';
    if (out_len) *out_len = olen;
    return out;
}

// ---- Simple JSON serialization for session data ----

static void session_json_append(char** buf, size_t* len, size_t* cap, const char* str, size_t slen) {
    while (*len + slen + 1 > *cap) {
        *cap = (*cap < 64) ? 64 : *cap * 2;
        *buf = (char*)realloc(*buf, *cap);
    }
    memcpy(*buf + *len, str, slen);
    *len += slen;
    (*buf)[*len] = '\0';
}

static void session_json_char(char** buf, size_t* len, size_t* cap, char c) {
    session_json_append(buf, len, cap, &c, 1);
}

static void session_json_encode_string(const char* str, size_t slen, char** buf, size_t* len, size_t* cap) {
    session_json_char(buf, len, cap, '"');
    for (size_t i = 0; i < slen; i++) {
        char c = str[i];
        switch (c) {
            case '"':  session_json_append(buf, len, cap, "\\\"", 2); break;
            case '\\': session_json_append(buf, len, cap, "\\\\", 2); break;
            case '\n': session_json_append(buf, len, cap, "\\n", 2); break;
            case '\r': session_json_append(buf, len, cap, "\\r", 2); break;
            case '\t': session_json_append(buf, len, cap, "\\t", 2); break;
            default:   session_json_char(buf, len, cap, c); break;
        }
    }
    session_json_char(buf, len, cap, '"');
}

// Encode a Value as JSON (subset: string, int, float, bool, nil)
static void session_json_encode_value(Value value, char** buf, size_t* len, size_t* cap) {
    if (IS_NIL(value)) {
        session_json_append(buf, len, cap, "null", 4);
    } else if (IS_BOOL(value)) {
        if (AS_BOOL(value)) {
            session_json_append(buf, len, cap, "true", 4);
        } else {
            session_json_append(buf, len, cap, "false", 5);
        }
    } else if (IS_INT(value)) {
        char num[32];
        int nlen = snprintf(num, sizeof(num), "%lld", (long long)AS_INT(value));
        session_json_append(buf, len, cap, num, (size_t)nlen);
    } else if (IS_FLOAT(value)) {
        char num[64];
        int nlen = snprintf(num, sizeof(num), "%g", AS_DOUBLE(value));
        session_json_append(buf, len, cap, num, (size_t)nlen);
    } else if (IS_STRING(value)) {
        ObjString* s = AS_STRING(value);
        session_json_encode_string(s->data, s->length, buf, len, cap);
    } else {
        session_json_append(buf, len, cap, "null", 4);
    }
}

// Encode session map as JSON object
static char* session_map_to_json(ObjMap* map, size_t* json_len) {
    char* buf = NULL;
    size_t len = 0, cap = 0;

    session_json_char(&buf, &len, &cap, '{');

    bool first = true;
    for (uint32_t i = 0; i < map->capacity; i++) {
        if (map->entries[i].key == NULL || map->entries[i].key == MAP_TOMBSTONE) continue;
        if (!first) session_json_char(&buf, &len, &cap, ',');
        first = false;

        ObjString* key = map->entries[i].key;
        session_json_encode_string(key->data, key->length, &buf, &len, &cap);
        session_json_char(&buf, &len, &cap, ':');
        session_json_encode_value(map->entries[i].value, &buf, &len, &cap);
    }

    session_json_char(&buf, &len, &cap, '}');

    if (json_len) *json_len = len;
    return buf;
}

// ---- Simple JSON parser for session data ----

typedef struct {
    const char* data;
    size_t len;
    size_t pos;
} JsonParser;

static void json_skip_ws(JsonParser* p) {
    while (p->pos < p->len && (p->data[p->pos] == ' ' || p->data[p->pos] == '\t' ||
           p->data[p->pos] == '\n' || p->data[p->pos] == '\r')) {
        p->pos++;
    }
}

static bool json_match(JsonParser* p, char c) {
    json_skip_ws(p);
    if (p->pos < p->len && p->data[p->pos] == c) {
        p->pos++;
        return true;
    }
    return false;
}

static Value json_parse_value(JsonParser* p);

static Value json_parse_string(JsonParser* p) {
    if (p->pos >= p->len || p->data[p->pos] != '"') return VAL_NIL;
    p->pos++; // skip opening "

    char* buf = NULL;
    size_t len = 0, cap = 0;

    while (p->pos < p->len && p->data[p->pos] != '"') {
        char c = p->data[p->pos++];
        if (c == '\\' && p->pos < p->len) {
            char esc = p->data[p->pos++];
            switch (esc) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                default: c = esc; break;
            }
        }
        session_json_append(&buf, &len, &cap, &c, 1);
    }
    if (p->pos < p->len) p->pos++; // skip closing "

    ObjString* str = obj_string_new(buf ? buf : "", (uint32_t)len);
    free(buf);
    return OBJ_VAL(str);
}

static Value json_parse_number(JsonParser* p) {
    const char* start = p->data + p->pos;
    bool is_float = false;
    bool negative = false;

    if (p->pos < p->len && p->data[p->pos] == '-') {
        negative = true;
        p->pos++;
    }

    while (p->pos < p->len && p->data[p->pos] >= '0' && p->data[p->pos] <= '9') {
        p->pos++;
    }
    if (p->pos < p->len && p->data[p->pos] == '.') {
        is_float = true;
        p->pos++;
        while (p->pos < p->len && p->data[p->pos] >= '0' && p->data[p->pos] <= '9') {
            p->pos++;
        }
    }

    (void)negative;
    if (is_float) {
        double val = strtod(start, NULL);
        return FLOAT_VAL(val);
    } else {
        int64_t val = strtoll(start, NULL, 10);
        return INT_VAL(val);
    }
}

static Value json_parse_value(JsonParser* p) {
    json_skip_ws(p);
    if (p->pos >= p->len) return VAL_NIL;

    char c = p->data[p->pos];
    if (c == '"') {
        return json_parse_string(p);
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        return json_parse_number(p);
    } else if (c == 't' && p->pos + 4 <= p->len && memcmp(p->data + p->pos, "true", 4) == 0) {
        p->pos += 4;
        return BOOL_VAL(true);
    } else if (c == 'f' && p->pos + 5 <= p->len && memcmp(p->data + p->pos, "false", 5) == 0) {
        p->pos += 5;
        return BOOL_VAL(false);
    } else if (c == 'n' && p->pos + 4 <= p->len && memcmp(p->data + p->pos, "null", 4) == 0) {
        p->pos += 4;
        return VAL_NIL;
    }
    return VAL_NIL;
}

// Parse a JSON object into an ObjMap
static ObjMap* json_parse_object(JsonParser* p) {
    if (!json_match(p, '{')) return NULL;

    ObjMap* map = obj_map_new();
    gc_push_root(OBJ_VAL(map));

    json_skip_ws(p);
    if (p->pos < p->len && p->data[p->pos] == '}') {
        p->pos++;
        gc_pop_root();
        return map;
    }

    while (p->pos < p->len) {
        json_skip_ws(p);
        // parse key (must be string)
        Value key_val = json_parse_string(p);
        if (!IS_STRING(key_val)) break;
        ObjString* key = AS_STRING(key_val);

        json_skip_ws(p);
        if (!json_match(p, ':')) break;

        Value val = json_parse_value(p);
        obj_map_set(map, key, val);

        json_skip_ws(p);
        if (p->pos < p->len && p->data[p->pos] == ',') {
            p->pos++;
        } else {
            break;
        }
    }

    json_match(p, '}');
    gc_pop_root();
    return map;
}

// ---- Ensure session maps exist ----
static void ensure_session_data(void) {
    if (!session_data) {
        session_data = obj_map_new();
        vm_pin((ObjHeader*)session_data);
    }
    if (!flash_data) {
        flash_data = obj_map_new();
        vm_pin((ObjHeader*)flash_data);
    }
}

// ---- Native functions ----

// session.init(secret_string) - sets the HMAC signing key
static Value native_session_init(int argc, Value* args) {
    (void)argc;
    if (!IS_STRING(args[0])) return VAL_NIL;

    ObjString* secret = AS_STRING(args[0]);
    if (session_secret) free(session_secret);
    session_secret = (char*)malloc(secret->length);
    memcpy(session_secret, secret->data, secret->length);
    session_secret_len = secret->length;

    ensure_session_data();
    return VAL_TRUE;
}

// session.set(key, value) - stores key-value in current session
static Value native_session_set(int argc, Value* args) {
    (void)argc;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ensure_session_data();

    ObjString* key = AS_STRING(args[0]);
    obj_map_set(session_data, key, args[1]);
    return VAL_TRUE;
}

// session.get(key) - retrieves value from session, or nil
static Value native_session_get(int argc, Value* args) {
    (void)argc;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ensure_session_data();

    ObjString* key = AS_STRING(args[0]);
    Value val;
    if (obj_map_get(session_data, key, &val)) {
        return val;
    }
    return VAL_NIL;
}

// session.delete(key) - removes key from session
static Value native_session_delete(int argc, Value* args) {
    (void)argc;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ensure_session_data();

    ObjString* key = AS_STRING(args[0]);
    obj_map_delete(session_data, key);
    return VAL_TRUE;
}

// session.flash_set(key, value) - sets a flash message
static Value native_session_flash_set(int argc, Value* args) {
    (void)argc;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ensure_session_data();

    ObjString* key = AS_STRING(args[0]);
    obj_map_set(flash_data, key, args[1]);
    return VAL_TRUE;
}

// session.flash_get(key) - gets and removes flash message
static Value native_session_flash_get(int argc, Value* args) {
    (void)argc;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ensure_session_data();

    ObjString* key = AS_STRING(args[0]);
    Value val;
    if (obj_map_get(flash_data, key, &val)) {
        obj_map_delete(flash_data, key);
        return val;
    }
    return VAL_NIL;
}

// session.encode() - serializes session data to signed cookie string
// Format: base64(json_data).base64(hmac_signature)
static Value native_session_encode(int argc, Value* args) {
    (void)argc;
    (void)args;
    ensure_session_data();

    if (!session_secret || session_secret_len == 0) {
        return VAL_NIL;
    }

    // Serialize session data to JSON
    size_t json_len = 0;
    char* json = session_map_to_json(session_data, &json_len);
    if (!json) return VAL_NIL;

    // Base64 encode JSON
    size_t b64_data_len = 0;
    char* b64_data = base64_encode((const uint8_t*)json, json_len, &b64_data_len);
    free(json);
    if (!b64_data) return VAL_NIL;

    // Compute HMAC-SHA256 of the base64-encoded data
    uint8_t hmac[32];
    hmac_sha256((const uint8_t*)session_secret, session_secret_len,
                (const uint8_t*)b64_data, b64_data_len, hmac);

    // Base64 encode HMAC
    size_t b64_hmac_len = 0;
    char* b64_hmac = base64_encode(hmac, 32, &b64_hmac_len);
    if (!b64_hmac) { free(b64_data); return VAL_NIL; }

    // Build result: data.signature
    size_t total_len = b64_data_len + 1 + b64_hmac_len;
    char* result = (char*)malloc(total_len + 1);
    memcpy(result, b64_data, b64_data_len);
    result[b64_data_len] = '.';
    memcpy(result + b64_data_len + 1, b64_hmac, b64_hmac_len);
    result[total_len] = '\0';

    ObjString* str = obj_string_new(result, (uint32_t)total_len);
    free(b64_data);
    free(b64_hmac);
    free(result);

    return OBJ_VAL(str);
}

// session.decode(cookie_string) - verifies signature and deserializes session
static Value native_session_decode(int argc, Value* args) {
    (void)argc;
    if (!IS_STRING(args[0])) return VAL_NIL;
    if (!session_secret || session_secret_len == 0) return VAL_NIL;

    ObjString* cookie = AS_STRING(args[0]);

    // Find the '.' separator
    const char* dot = NULL;
    for (uint32_t i = 0; i < cookie->length; i++) {
        if (cookie->data[i] == '.') {
            dot = cookie->data + i;
            break;
        }
    }
    if (!dot) return VAL_NIL;

    size_t data_len = (size_t)(dot - cookie->data);
    size_t sig_len = cookie->length - data_len - 1;

    // Compute expected HMAC
    uint8_t expected_hmac[32];
    hmac_sha256((const uint8_t*)session_secret, session_secret_len,
                (const uint8_t*)cookie->data, data_len, expected_hmac);

    // Base64 decode the provided signature
    size_t decoded_sig_len = 0;
    uint8_t* decoded_sig = base64_decode(dot + 1, sig_len, &decoded_sig_len);
    if (!decoded_sig || decoded_sig_len != 32) {
        free(decoded_sig);
        return VAL_NIL;
    }

    // Constant-time comparison
    int diff = 0;
    for (int i = 0; i < 32; i++) {
        diff |= expected_hmac[i] ^ decoded_sig[i];
    }
    free(decoded_sig);
    if (diff != 0) return VAL_NIL;

    // Signature valid - decode the data
    size_t decoded_data_len = 0;
    uint8_t* decoded_data = base64_decode(cookie->data, data_len, &decoded_data_len);
    if (!decoded_data) return VAL_NIL;

    // Parse JSON into session map
    JsonParser parser = { .data = (const char*)decoded_data, .len = decoded_data_len, .pos = 0 };
    ObjMap* new_data = json_parse_object(&parser);
    free(decoded_data);

    if (!new_data) return VAL_NIL;

    // Replace session data
    session_data = new_data;
    vm_pin((ObjHeader*)session_data);

    return VAL_TRUE;
}

void stdlib_session_init(ObjMap* pkg) {
    stdlib_register(pkg, "init", native_session_init, 1);
    stdlib_register(pkg, "set", native_session_set, 2);
    stdlib_register(pkg, "get", native_session_get, 1);
    stdlib_register(pkg, "delete", native_session_delete, 1);
    stdlib_register(pkg, "flash_set", native_session_flash_set, 2);
    stdlib_register(pkg, "flash_get", native_session_flash_get, 1);
    stdlib_register(pkg, "encode", native_session_encode, 0);
    stdlib_register(pkg, "decode", native_session_decode, 1);
}
