/*
 * vekd_crypto.c - Master key management and encryption implementation.
 *
 * Uses a simple XOR stream cipher keyed with SHA256-derived keystream.
 * The master key is 32 bytes read from /dev/urandom on generation.
 */
#include "vekd_crypto.h"
#include "vekd_config.h"
#include "../sha256.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

int vekd_crypto_load_key(const char *path, uint8_t key[VEKD_KEY_SIZE]) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    ssize_t n = read(fd, key, VEKD_KEY_SIZE);
    close(fd);

    if (n != VEKD_KEY_SIZE) return -1;
    return 0;
}

int vekd_crypto_generate_key(const char *path, uint8_t key[VEKD_KEY_SIZE]) {
    /* Read random bytes from /dev/urandom */
    int rfd = open("/dev/urandom", O_RDONLY);
    if (rfd < 0) {
        fprintf(stderr, "vekd: cannot open /dev/urandom: %s\n", strerror(errno));
        return -1;
    }

    ssize_t n = read(rfd, key, VEKD_KEY_SIZE);
    close(rfd);
    if (n != VEKD_KEY_SIZE) {
        fprintf(stderr, "vekd: failed to read random bytes\n");
        return -1;
    }

    /* Write the key file with mode 0600 */
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, VEKD_MASTER_KEY_MODE);
    if (fd < 0) {
        fprintf(stderr, "vekd: cannot create key file %s: %s\n", path, strerror(errno));
        return -1;
    }

    n = write(fd, key, VEKD_KEY_SIZE);
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
        /* Key file exists, load it */
        return vekd_crypto_load_key(path, key);
    }
    /* Key file does not exist, generate new one */
    return vekd_crypto_generate_key(path, key);
}

/*
 * Simple XOR stream cipher using SHA256 to expand the key.
 * Each 32-byte block of output is XORed with SHA256(key || block_counter).
 * This is symmetric: encrypt and decrypt are the same operation.
 */
int vekd_crypto_encrypt(const uint8_t key[VEKD_KEY_SIZE],
                        const uint8_t *input, size_t input_len,
                        uint8_t *output) {
    uint8_t block_input[VEKD_KEY_SIZE + 4]; /* key + 4-byte counter */
    uint8_t keystream[32];
    size_t offset = 0;
    uint32_t counter = 0;

    memcpy(block_input, key, VEKD_KEY_SIZE);

    while (offset < input_len) {
        /* Encode counter into block_input */
        block_input[VEKD_KEY_SIZE]     = (uint8_t)(counter >> 24);
        block_input[VEKD_KEY_SIZE + 1] = (uint8_t)(counter >> 16);
        block_input[VEKD_KEY_SIZE + 2] = (uint8_t)(counter >> 8);
        block_input[VEKD_KEY_SIZE + 3] = (uint8_t)(counter);

        sha256_compute(block_input, sizeof(block_input), keystream);

        size_t chunk = input_len - offset;
        if (chunk > 32) chunk = 32;

        for (size_t i = 0; i < chunk; i++) {
            output[offset + i] = input[offset + i] ^ keystream[i];
        }

        offset += chunk;
        counter++;
    }

    return 0;
}

int vekd_crypto_decrypt(const uint8_t key[VEKD_KEY_SIZE],
                        const uint8_t *input, size_t input_len,
                        uint8_t *output) {
    /* Symmetric cipher - decrypt is identical to encrypt */
    return vekd_crypto_encrypt(key, input, input_len, output);
}
