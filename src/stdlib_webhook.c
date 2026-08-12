#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

// External HMAC-SHA256 from stdlib_session.c
extern void hmac_sha256(const uint8_t* key, size_t key_len,
                        const uint8_t* msg, size_t msg_len,
                        uint8_t out[32]);

// ---- Hex encoding helper ----

static void bytes_to_hex(const uint8_t* data, size_t len, char* out) {
    static const char hex_chars[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = hex_chars[(data[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex_chars[data[i] & 0xF];
    }
    out[len * 2] = '\0';
}

// ---- Constant-time compare ----

static bool constant_time_eq(const char* a, const char* b, size_t len) {
    uint8_t result = 0;
    for (size_t i = 0; i < len; i++) {
        result |= (uint8_t)(a[i] ^ b[i]);
    }
    return result == 0;
}

// ---- webhook.verify(payload, signature, secret) ----
// Generic HMAC-SHA256 verification: computes HMAC of payload with secret,
// compares hex result to signature.

static Value native_webhook_verify(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0]) || !IS_STRING(args[1]) || !IS_STRING(args[2])) {
        return VAL_FALSE;
    }

    ObjString* payload = AS_STRING(args[0]);
    ObjString* signature = AS_STRING(args[1]);
    ObjString* secret = AS_STRING(args[2]);

    uint8_t hmac[32];
    hmac_sha256((const uint8_t*)secret->data, secret->length,
                (const uint8_t*)payload->data, payload->length,
                hmac);

    char hex[65];
    bytes_to_hex(hmac, 32, hex);

    // Compare - signature should be 64 hex chars
    if (signature->length != 64) return VAL_FALSE;

    return constant_time_eq(hex, signature->data, 64) ? VAL_TRUE : VAL_FALSE;
}

// ---- webhook.verify_github(payload, signature, secret) ----
// Verifies X-Hub-Signature-256 format: "sha256=<hex_hmac>"

static Value native_webhook_verify_github(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0]) || !IS_STRING(args[1]) || !IS_STRING(args[2])) {
        return VAL_FALSE;
    }

    ObjString* payload = AS_STRING(args[0]);
    ObjString* signature = AS_STRING(args[1]);
    ObjString* secret = AS_STRING(args[2]);

    // Signature format: "sha256=<64 hex chars>"
    if (signature->length != 71) return VAL_FALSE;
    if (memcmp(signature->data, "sha256=", 7) != 0) return VAL_FALSE;

    uint8_t hmac[32];
    hmac_sha256((const uint8_t*)secret->data, secret->length,
                (const uint8_t*)payload->data, payload->length,
                hmac);

    char hex[65];
    bytes_to_hex(hmac, 32, hex);

    return constant_time_eq(hex, signature->data + 7, 64) ? VAL_TRUE : VAL_FALSE;
}

// ---- webhook.verify_stripe(payload, signature_header, secret) ----
// Stripe signature format: "t=<timestamp>,v1=<hmac_hex>"
// Signed message is: "<timestamp>.<payload>"

static Value native_webhook_verify_stripe(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0]) || !IS_STRING(args[1]) || !IS_STRING(args[2])) {
        return VAL_FALSE;
    }

    ObjString* payload = AS_STRING(args[0]);
    ObjString* sig_header = AS_STRING(args[1]);
    ObjString* secret = AS_STRING(args[2]);

    // Parse signature header: "t=<timestamp>,v1=<hex>"
    const char* s = sig_header->data;
    size_t slen = sig_header->length;

    // Find t= value
    if (slen < 2 || s[0] != 't' || s[1] != '=') return VAL_FALSE;

    size_t ts_start = 2;
    size_t ts_end = ts_start;
    while (ts_end < slen && s[ts_end] != ',') ts_end++;
    size_t ts_len = ts_end - ts_start;

    if (ts_end >= slen) return VAL_FALSE; // no comma found

    // Find v1= value
    size_t v1_pos = ts_end + 1;
    if (v1_pos + 2 >= slen || s[v1_pos] != 'v' || s[v1_pos + 1] != '1' || s[v1_pos + 2] != '=') {
        return VAL_FALSE;
    }
    size_t hmac_start = v1_pos + 3;
    size_t hmac_len = slen - hmac_start;

    if (hmac_len != 64) return VAL_FALSE;

    // Construct signed payload: "<timestamp>.<payload>"
    size_t msg_len = ts_len + 1 + payload->length;
    char* msg = (char*)malloc(msg_len);
    if (!msg) return VAL_FALSE;

    memcpy(msg, s + ts_start, ts_len);
    msg[ts_len] = '.';
    memcpy(msg + ts_len + 1, payload->data, payload->length);

    uint8_t hmac[32];
    hmac_sha256((const uint8_t*)secret->data, secret->length,
                (const uint8_t*)msg, msg_len,
                hmac);
    free(msg);

    char hex[65];
    bytes_to_hex(hmac, 32, hex);

    return constant_time_eq(hex, s + hmac_start, 64) ? VAL_TRUE : VAL_FALSE;
}

void stdlib_webhook_init(ObjMap* pkg) {
    stdlib_register(pkg, "verify", native_webhook_verify, 3);
    stdlib_register(pkg, "verify_github", native_webhook_verify_github, 3);
    stdlib_register(pkg, "verify_stripe", native_webhook_verify_stripe, 3);
}
