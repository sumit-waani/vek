/*
 * vekd_crypto.c - Master key management, encryption, and password hashing.
 *
 * Encryption: XOR stream cipher keyed with SHA256(key || nonce || counter).
 * A random 16-byte nonce is prepended to every ciphertext to ensure that
 * identical plaintexts produce different outputs.
 *
 * Password hashing: PBKDF2-SHA256 with a random 16-byte salt and 100k iterations.
 * Stored format: hex(salt) + "$" + hex(derived_key).
 */
#include "vekd_crypto.h"
#include "vekd_config.h"
#include "../sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

/* --- Random bytes --- */

int vekd_crypto_random_bytes(uint8_t *buf, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, len);
    close(fd);
    return (n == (ssize_t)len) ? 0 : -1;
}

/* --- Key management --- */

int vekd_crypto_load_key(const char *path, uint8_t key[VEKD_KEY_SIZE]) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    ssize_t n = read(fd, key, VEKD_KEY_SIZE);
    close(fd);

    if (n != VEKD_KEY_SIZE) return -1;
    return 0;
}

int vekd_crypto_generate_key(const char *path, uint8_t key[VEKD_KEY_SIZE]) {
    if (vekd_crypto_random_bytes(key, VEKD_KEY_SIZE) < 0) {
        fprintf(stderr, "vekd: failed to read random bytes for key\n");
        return -1;
    }

    /* Write the key file with mode 0600 */
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, VEKD_MASTER_KEY_MODE);
    if (fd < 0) {
        fprintf(stderr, "vekd: cannot create key file %s: %s\n", path, strerror(errno));
        return -1;
    }

    ssize_t n = write(fd, key, VEKD_KEY_SIZE);
    close(fd);
    if (n != VEKD_KEY_SIZE) {
        fprintf(stderr, "vekd: failed to write key file\n");
        return -1;
    }

    return 0;
}

int vekd_crypto_init_key(const char *path, uint8_t key[VEKD_KEY_SIZE]) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return vekd_crypto_load_key(path, key);
    }
    return vekd_crypto_generate_key(path, key);
}

/* --- Stream cipher core (internal) --- */

/*
 * Generate keystream and XOR with input.
 * Uses SHA256(key || nonce || counter) per 32-byte block.
 */
static void xor_stream(const uint8_t key[VEKD_KEY_SIZE],
                       const uint8_t nonce[VEKD_NONCE_SIZE],
                       const uint8_t *input, size_t input_len,
                       uint8_t *output) {
    /* block_input = key(32) + nonce(16) + counter(4) = 52 bytes */
    uint8_t block_input[VEKD_KEY_SIZE + VEKD_NONCE_SIZE + 4];
    uint8_t keystream[32];
    size_t offset = 0;
    uint32_t counter = 0;

    memcpy(block_input, key, VEKD_KEY_SIZE);
    memcpy(block_input + VEKD_KEY_SIZE, nonce, VEKD_NONCE_SIZE);

    while (offset < input_len) {
        /* Encode counter */
        block_input[VEKD_KEY_SIZE + VEKD_NONCE_SIZE]     = (uint8_t)(counter >> 24);
        block_input[VEKD_KEY_SIZE + VEKD_NONCE_SIZE + 1] = (uint8_t)(counter >> 16);
        block_input[VEKD_KEY_SIZE + VEKD_NONCE_SIZE + 2] = (uint8_t)(counter >> 8);
        block_input[VEKD_KEY_SIZE + VEKD_NONCE_SIZE + 3] = (uint8_t)(counter);

        sha256_compute(block_input, sizeof(block_input), keystream);

        size_t chunk = input_len - offset;
        if (chunk > 32) chunk = 32;

        for (size_t i = 0; i < chunk; i++) {
            output[offset + i] = input[offset + i] ^ keystream[i];
        }

        offset += chunk;
        counter++;
    }
}

/* --- Encrypt / Decrypt (with nonce) --- */

int vekd_crypto_encrypt(const uint8_t key[VEKD_KEY_SIZE],
                        const uint8_t *input, size_t input_len,
                        uint8_t *output) {
    /* Generate random nonce and write it as prefix */
    if (vekd_crypto_random_bytes(output, VEKD_NONCE_SIZE) < 0) {
        return -1;
    }

    /* Encrypt data after the nonce prefix */
    xor_stream(key, output, input, input_len, output + VEKD_NONCE_SIZE);
    return 0;
}

int vekd_crypto_decrypt(const uint8_t key[VEKD_KEY_SIZE],
                        const uint8_t *input, size_t input_len,
                        uint8_t *output) {
    if (input_len < VEKD_NONCE_SIZE) return -1;

    /* Read nonce from prefix */
    const uint8_t *nonce = input;
    const uint8_t *ciphertext = input + VEKD_NONCE_SIZE;
    size_t ct_len = input_len - VEKD_NONCE_SIZE;

    xor_stream(key, nonce, ciphertext, ct_len, output);
    return 0;
}

/* --- PBKDF2-SHA256 --- */

/*
 * HMAC-SHA256 implementation using the existing sha256_compute.
 */
static void hmac_sha256(const uint8_t *key, size_t key_len,
                        const uint8_t *data, size_t data_len,
                        uint8_t out[32]) {
    uint8_t k_pad[64];
    uint8_t temp_key[32];

    /* If key is longer than block size, hash it */
    if (key_len > 64) {
        sha256_compute(key, key_len, temp_key);
        key = temp_key;
        key_len = 32;
    }

    /* Inner pad */
    memset(k_pad, 0x36, 64);
    for (size_t i = 0; i < key_len; i++) {
        k_pad[i] ^= key[i];
    }

    /* inner_hash = SHA256(k_pad || data) */
    uint8_t inner_buf[64 + 1024]; /* stack buffer for small data */
    uint8_t *inner_alloc = NULL;
    uint8_t *inner;
    if (64 + data_len <= sizeof(inner_buf)) {
        inner = inner_buf;
    } else {
        inner_alloc = (uint8_t *)malloc(64 + data_len);
        if (!inner_alloc) { memset(out, 0, 32); return; }
        inner = inner_alloc;
    }
    memcpy(inner, k_pad, 64);
    memcpy(inner + 64, data, data_len);

    uint8_t inner_hash[32];
    sha256_compute(inner, 64 + data_len, inner_hash);

    if (inner_alloc) { free(inner_alloc); inner_alloc = NULL; }

    /* Outer pad */
    memset(k_pad, 0x5c, 64);
    for (size_t i = 0; i < key_len; i++) {
        k_pad[i] ^= key[i];
    }

    /* SHA256(k_pad || inner_hash) */
    uint8_t outer[64 + 32];
    memcpy(outer, k_pad, 64);
    memcpy(outer + 64, inner_hash, 32);
    sha256_compute(outer, 96, out);
}

/*
 * PBKDF2-SHA256: derive a 32-byte key from password + salt.
 */
static void pbkdf2_sha256(const char *password, size_t pass_len,
                          const uint8_t *salt, size_t salt_len,
                          uint32_t iterations, uint8_t out[32]) {
    /* We only need one block (32 bytes output) */
    /* U1 = HMAC(password, salt || INT_32_BE(1)) */
    uint8_t salt_block[256];
    if (salt_len + 4 > sizeof(salt_block)) {
        /* Extremely long salt - just truncate for safety */
        salt_len = sizeof(salt_block) - 4;
    }
    memcpy(salt_block, salt, salt_len);
    salt_block[salt_len]     = 0;
    salt_block[salt_len + 1] = 0;
    salt_block[salt_len + 2] = 0;
    salt_block[salt_len + 3] = 1;

    uint8_t u[32];
    hmac_sha256((const uint8_t *)password, pass_len,
                salt_block, salt_len + 4, u);
    memcpy(out, u, 32);

    /* Subsequent iterations */
    for (uint32_t i = 1; i < iterations; i++) {
        uint8_t u_next[32];
        hmac_sha256((const uint8_t *)password, pass_len, u, 32, u_next);
        memcpy(u, u_next, 32);
        for (int j = 0; j < 32; j++) {
            out[j] ^= u[j];
        }
    }
}

/* --- Password hashing --- */

static void bytes_to_hex(const uint8_t *bytes, size_t len, char *hex) {
    for (size_t i = 0; i < len; i++) {
        snprintf(hex + (i * 2), 3, "%02x", bytes[i]);
    }
}

static int hex_to_bytes(const char *hex, size_t hex_len, uint8_t *bytes, size_t bytes_len) {
    if (hex_len % 2 != 0) return -1;
    size_t num_bytes = hex_len / 2;
    if (num_bytes > bytes_len) return -1;

    for (size_t i = 0; i < num_bytes; i++) {
        unsigned int val;
        char byte_str[3] = { hex[i*2], hex[i*2+1], '\0' };
        if (sscanf(byte_str, "%02x", &val) != 1) return -1;
        bytes[i] = (uint8_t)val;
    }
    return (int)num_bytes;
}

void vekd_crypto_hash_password(const char *password, char *out_hash, size_t hash_len) {
    /* Generate random salt */
    uint8_t salt[VEKD_SALT_SIZE];
    vekd_crypto_random_bytes(salt, VEKD_SALT_SIZE);

    /* Derive key */
    uint8_t derived[32];
    pbkdf2_sha256(password, strlen(password), salt, VEKD_SALT_SIZE,
                  VEKD_PBKDF2_ITERATIONS, derived);

    /* Format: hex(salt) + "$" + hex(derived) */
    /* salt=16 bytes=32 hex chars, derived=32 bytes=64 hex chars, + "$" + null = 98 */
    if (hash_len < 98) {
        out_hash[0] = '\0';
        return;
    }
    bytes_to_hex(salt, VEKD_SALT_SIZE, out_hash);
    out_hash[32] = '$';
    bytes_to_hex(derived, 32, out_hash + 33);
    out_hash[97] = '\0';
}

bool vekd_crypto_verify_password(const char *password, const char *stored_hash) {
    /* Parse stored hash: hex(salt)$hex(derived) */
    size_t hash_len = strlen(stored_hash);
    if (hash_len < 97) return false;

    /* Find the '$' separator at position 32 */
    if (stored_hash[32] != '$') return false;

    /* Parse salt */
    uint8_t salt[VEKD_SALT_SIZE];
    if (hex_to_bytes(stored_hash, 32, salt, VEKD_SALT_SIZE) != VEKD_SALT_SIZE) {
        return false;
    }

    /* Parse expected derived key */
    uint8_t expected[32];
    if (hex_to_bytes(stored_hash + 33, 64, expected, 32) != 32) {
        return false;
    }

    /* Derive from the provided password with the same salt */
    uint8_t derived[32];
    pbkdf2_sha256(password, strlen(password), salt, VEKD_SALT_SIZE,
                  VEKD_PBKDF2_ITERATIONS, derived);

    /* Constant-time comparison */
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) {
        diff |= derived[i] ^ expected[i];
    }
    return diff == 0;
}
