#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

#include <fcntl.h>
#include <unistd.h>
#include <time.h>

// ---- SHA-256 implementation (for PBKDF2) ----

static const uint32_t auth_sha256_k[64] = {
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

#define AUTH_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define AUTH_CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define AUTH_MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define AUTH_EP0(x) (AUTH_ROTR(x, 2) ^ AUTH_ROTR(x, 13) ^ AUTH_ROTR(x, 22))
#define AUTH_EP1(x) (AUTH_ROTR(x, 6) ^ AUTH_ROTR(x, 11) ^ AUTH_ROTR(x, 25))
#define AUTH_SIG0(x) (AUTH_ROTR(x, 7) ^ AUTH_ROTR(x, 18) ^ ((x) >> 3))
#define AUTH_SIG1(x) (AUTH_ROTR(x, 17) ^ AUTH_ROTR(x, 19) ^ ((x) >> 10))

static void auth_sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
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
            w[i] = AUTH_SIG1(w[i-2]) + w[i-7] + AUTH_SIG0(w[i-15]) + w[i-16];
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + AUTH_EP1(e) + AUTH_CH(e, f, g) + auth_sha256_k[i] + w[i];
            uint32_t t2 = AUTH_EP0(a) + AUTH_MAJ(a, b, c);
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

static void auth_hmac_sha256(const uint8_t* key, size_t key_len,
                             const uint8_t* msg, size_t msg_len,
                             uint8_t out[32]) {
    uint8_t k[64];
    memset(k, 0, 64);

    if (key_len > 64) {
        auth_sha256(key, key_len, k);
    } else {
        memcpy(k, key, key_len);
    }

    uint8_t ipad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
    }

    uint8_t opad[64];
    for (int i = 0; i < 64; i++) {
        opad[i] = k[i] ^ 0x5c;
    }

    size_t inner_len = 64 + msg_len;
    uint8_t* inner_data = (uint8_t*)malloc(inner_len);
    memcpy(inner_data, ipad, 64);
    memcpy(inner_data + 64, msg, msg_len);

    uint8_t inner_hash[32];
    auth_sha256(inner_data, inner_len, inner_hash);
    free(inner_data);

    uint8_t outer_data[96];
    memcpy(outer_data, opad, 64);
    memcpy(outer_data + 64, inner_hash, 32);

    auth_sha256(outer_data, 96, out);
}

// ---- PBKDF2-SHA256 ----

static void auth_pbkdf2_sha256(const uint8_t* password, size_t pass_len,
                               const uint8_t* salt, size_t salt_len,
                               uint32_t iterations, uint8_t* out, size_t out_len) {
    uint32_t block_num = 1;
    size_t offset = 0;

    while (offset < out_len) {
        // U1 = HMAC(password, salt || INT_32_BE(block_num))
        size_t msg_len = salt_len + 4;
        uint8_t* msg = (uint8_t*)malloc(msg_len);
        memcpy(msg, salt, salt_len);
        msg[salt_len]     = (uint8_t)(block_num >> 24);
        msg[salt_len + 1] = (uint8_t)(block_num >> 16);
        msg[salt_len + 2] = (uint8_t)(block_num >> 8);
        msg[salt_len + 3] = (uint8_t)(block_num);

        uint8_t u[32];
        auth_hmac_sha256(password, pass_len, msg, msg_len, u);
        free(msg);

        uint8_t result[32];
        memcpy(result, u, 32);

        for (uint32_t i = 1; i < iterations; i++) {
            auth_hmac_sha256(password, pass_len, u, 32, u);
            for (int j = 0; j < 32; j++) {
                result[j] ^= u[j];
            }
        }

        size_t to_copy = out_len - offset;
        if (to_copy > 32) to_copy = 32;
        memcpy(out + offset, result, to_copy);

        offset += to_copy;
        block_num++;
    }
}

// ---- Helper: read random bytes ----

static bool auth_random_bytes(uint8_t* buf, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;

    size_t total = 0;
    while (total < n) {
        ssize_t r = read(fd, buf + total, n - total);
        if (r <= 0) { close(fd); return false; }
        total += (size_t)r;
    }
    close(fd);
    return true;
}

// Hash format: "$pbkdf2-sha256$iterations$salt_hex$hash_hex"
// iterations = 10000, salt = 16 bytes

#define AUTH_ITERATIONS 10000
#define AUTH_SALT_LEN   16
#define AUTH_HASH_LEN   32

// auth.hash(password) - returns PBKDF2-SHA256 hash string
static Value native_auth_hash(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ObjString* password = AS_STRING(args[0]);

    // Generate random salt
    uint8_t salt[AUTH_SALT_LEN];
    if (!auth_random_bytes(salt, AUTH_SALT_LEN)) return VAL_NIL;

    // Derive key
    uint8_t derived[AUTH_HASH_LEN];
    auth_pbkdf2_sha256((const uint8_t*)password->data, password->length,
                       salt, AUTH_SALT_LEN, AUTH_ITERATIONS, derived, AUTH_HASH_LEN);

    // Format: $pbkdf2-sha256$10000$<salt_hex>$<hash_hex>
    // salt_hex = 32 chars, hash_hex = 64 chars
    char result[128];
    int pos = snprintf(result, sizeof(result), "$pbkdf2-sha256$%d$", AUTH_ITERATIONS);

    for (int i = 0; i < AUTH_SALT_LEN; i++) {
        snprintf(result + pos + i*2, 3, "%02x", salt[i]);
    }
    pos += AUTH_SALT_LEN * 2;

    result[pos++] = '$';

    for (int i = 0; i < AUTH_HASH_LEN; i++) {
        snprintf(result + pos + i*2, 3, "%02x", derived[i]);
    }
    pos += AUTH_HASH_LEN * 2;

    return OBJ_VAL(obj_string_new(result, (uint32_t)pos));
}

// Helper: parse hex byte
static int auth_hex_byte(const char* s) {
    int hi, lo;
    if (s[0] >= '0' && s[0] <= '9') hi = s[0] - '0';
    else if (s[0] >= 'a' && s[0] <= 'f') hi = s[0] - 'a' + 10;
    else if (s[0] >= 'A' && s[0] <= 'F') hi = s[0] - 'A' + 10;
    else return -1;

    if (s[1] >= '0' && s[1] <= '9') lo = s[1] - '0';
    else if (s[1] >= 'a' && s[1] <= 'f') lo = s[1] - 'a' + 10;
    else if (s[1] >= 'A' && s[1] <= 'F') lo = s[1] - 'A' + 10;
    else return -1;

    return (hi << 4) | lo;
}

// auth.verify(password, hash) - returns true/false
static Value native_auth_verify(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0]) || !IS_STRING(args[1])) return VAL_FALSE;
    ObjString* password = AS_STRING(args[0]);
    ObjString* hash_str = AS_STRING(args[1]);

    // Parse: $pbkdf2-sha256$<iterations>$<salt_hex>$<hash_hex>
    const char* s = hash_str->data;
    size_t slen = hash_str->length;

    // Must start with "$pbkdf2-sha256$"
    const char* prefix = "$pbkdf2-sha256$";
    size_t prefix_len = strlen(prefix);
    if (slen < prefix_len || memcmp(s, prefix, prefix_len) != 0) return VAL_FALSE;
    s += prefix_len;
    slen -= prefix_len;

    // Parse iterations
    uint32_t iterations = 0;
    while (slen > 0 && *s != '$') {
        if (*s < '0' || *s > '9') return VAL_FALSE;
        iterations = iterations * 10 + (uint32_t)(*s - '0');
        s++; slen--;
    }
    if (slen == 0 || *s != '$') return VAL_FALSE;
    s++; slen--;

    // Parse salt_hex (32 chars = 16 bytes)
    if (slen < AUTH_SALT_LEN * 2) return VAL_FALSE;
    uint8_t salt[AUTH_SALT_LEN];
    for (int i = 0; i < AUTH_SALT_LEN; i++) {
        int b = auth_hex_byte(s + i*2);
        if (b < 0) return VAL_FALSE;
        salt[i] = (uint8_t)b;
    }
    s += AUTH_SALT_LEN * 2;
    slen -= AUTH_SALT_LEN * 2;

    if (slen == 0 || *s != '$') return VAL_FALSE;
    s++; slen--;

    // Parse hash_hex (64 chars = 32 bytes)
    if (slen < AUTH_HASH_LEN * 2) return VAL_FALSE;
    uint8_t expected_hash[AUTH_HASH_LEN];
    for (int i = 0; i < AUTH_HASH_LEN; i++) {
        int b = auth_hex_byte(s + i*2);
        if (b < 0) return VAL_FALSE;
        expected_hash[i] = (uint8_t)b;
    }

    // Compute PBKDF2 with same parameters
    uint8_t derived[AUTH_HASH_LEN];
    auth_pbkdf2_sha256((const uint8_t*)password->data, password->length,
                       salt, AUTH_SALT_LEN, iterations, derived, AUTH_HASH_LEN);

    // Constant-time comparison
    int diff = 0;
    for (int i = 0; i < AUTH_HASH_LEN; i++) {
        diff |= derived[i] ^ expected_hash[i];
    }

    return diff == 0 ? VAL_TRUE : VAL_FALSE;
}

// auth.random_token(n) - returns hex string of n random bytes
static Value native_auth_random_token(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_INT(args[0])) return VAL_NIL;
    int64_t n = AS_INT(args[0]);
    if (n <= 0 || n > 256) return VAL_NIL;

    uint8_t* buf = (uint8_t*)malloc((size_t)n);
    if (!buf) return VAL_NIL;

    if (!auth_random_bytes(buf, (size_t)n)) {
        free(buf);
        return VAL_NIL;
    }

    // Convert to hex
    char* hex = (char*)malloc((size_t)n * 2 + 1);
    for (int64_t i = 0; i < n; i++) {
        snprintf(hex + i*2, 3, "%02x", buf[i]);
    }
    free(buf);

    ObjString* result = obj_string_new(hex, (uint32_t)(n * 2));
    free(hex);
    return OBJ_VAL(result);
}

void stdlib_auth_init(ObjMap* pkg) {
    stdlib_register(pkg, "hash", native_auth_hash, 1);
    stdlib_register(pkg, "verify", native_auth_verify, 2);
    stdlib_register(pkg, "random_token", native_auth_random_token, 1);
}
