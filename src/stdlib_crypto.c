#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

#include <fcntl.h>
#include <unistd.h>

// ---- SHA-256 implementation ----

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

static void sha256_hash(const uint8_t* data, size_t len, uint8_t out[32]) {
    uint32_t h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    // Pad message
    size_t padded_len = ((len + 8) / 64 + 1) * 64;
    uint8_t* msg = (uint8_t*)calloc(padded_len, 1);
    memcpy(msg, data, len);
    msg[len] = 0x80;
    // Length in bits (big-endian) at end
    uint64_t bit_len = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) {
        msg[padded_len - 1 - i] = (uint8_t)(bit_len >> (i * 8));
    }

    // Process blocks
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

    // Output hash
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(h[i] >> 24);
        out[i*4+1] = (uint8_t)(h[i] >> 16);
        out[i*4+2] = (uint8_t)(h[i] >> 8);
        out[i*4+3] = (uint8_t)(h[i]);
    }
}

// crypto.random_bytes(n) - returns ObjBytes of n random bytes from /dev/urandom
static Value native_crypto_random_bytes(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_INT(args[0])) {
        return VAL_NIL;
    }
    int64_t n = AS_INT(args[0]);
    if (n <= 0 || n > 65536) {
        return VAL_NIL;
    }

    uint8_t* buf = (uint8_t*)malloc((size_t)n);
    if (!buf) return VAL_NIL;

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        free(buf);
        return VAL_NIL;
    }

    size_t total = 0;
    while (total < (size_t)n) {
        ssize_t r = read(fd, buf + total, (size_t)n - total);
        if (r <= 0) { close(fd); free(buf); return VAL_NIL; }
        total += (size_t)r;
    }
    close(fd);

    ObjBytes* bytes = obj_bytes_new(buf, (uint32_t)n);
    free(buf);
    return OBJ_VAL(bytes);
}

// crypto.sha256(str) - returns hex string of SHA-256 hash
static Value native_crypto_sha256(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) {
        return VAL_NIL;
    }
    ObjString* input = AS_STRING(args[0]);

    uint8_t hash[32];
    sha256_hash((const uint8_t*)input->data, input->length, hash);

    // Convert to hex string
    char hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(hex + i*2, 3, "%02x", hash[i]);
    }
    return OBJ_VAL(obj_string_new(hex, 64));
}

void stdlib_crypto_init(ObjMap* pkg) {
    stdlib_register(pkg, "random_bytes", native_crypto_random_bytes, 1);
    stdlib_register(pkg, "sha256", native_crypto_sha256, 1);
}
