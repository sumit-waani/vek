/*
 * vekd_crypto.h - Master key management and encryption for vekd.
 *
 * The master key is stored at /var/lib/vek/master.key (mode 0600).
 * It is used to encrypt/decrypt secrets and environment variable values.
 */
#ifndef VEKD_CRYPTO_H
#define VEKD_CRYPTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define VEKD_KEY_SIZE 32

/* Load the master key from the given path. Returns 0 on success. */
int vekd_crypto_load_key(const char *path, uint8_t key[VEKD_KEY_SIZE]);

/* Generate a new master key and write it to the given path (mode 0600).
 * Returns 0 on success. */
int vekd_crypto_generate_key(const char *path, uint8_t key[VEKD_KEY_SIZE]);

/* Load or generate the master key at the given path.
 * If the file exists, load it. Otherwise, generate a new one.
 * Returns 0 on success. */
int vekd_crypto_init_key(const char *path, uint8_t key[VEKD_KEY_SIZE]);

/*
 * Encrypt data using the master key (XOR-based stream cipher with SHA256).
 * Output buffer must be at least input_len bytes.
 * Returns 0 on success.
 */
int vekd_crypto_encrypt(const uint8_t key[VEKD_KEY_SIZE],
                        const uint8_t *input, size_t input_len,
                        uint8_t *output);

/*
 * Decrypt data using the master key (symmetric - same as encrypt).
 * Output buffer must be at least input_len bytes.
 * Returns 0 on success.
 */
int vekd_crypto_decrypt(const uint8_t key[VEKD_KEY_SIZE],
                        const uint8_t *input, size_t input_len,
                        uint8_t *output);

#endif /* VEKD_CRYPTO_H */
