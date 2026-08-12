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
#define VEKD_NONCE_SIZE 16
#define VEKD_SALT_SIZE 16
#define VEKD_PBKDF2_ITERATIONS 100000

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
 * A random 16-byte nonce is prepended to the output.
 * Output buffer must be at least (VEKD_NONCE_SIZE + input_len) bytes.
 * Returns 0 on success.
 */
int vekd_crypto_encrypt(const uint8_t key[VEKD_KEY_SIZE],
                        const uint8_t *input, size_t input_len,
                        uint8_t *output);

/*
 * Decrypt data using the master key.
 * Input must begin with a 16-byte nonce prefix followed by ciphertext.
 * input_len must be >= VEKD_NONCE_SIZE.
 * Output buffer must be at least (input_len - VEKD_NONCE_SIZE) bytes.
 * Returns 0 on success, -1 if input_len is too small.
 */
int vekd_crypto_decrypt(const uint8_t key[VEKD_KEY_SIZE],
                        const uint8_t *input, size_t input_len,
                        uint8_t *output);

/*
 * Hash a password using PBKDF2-SHA256 with a random salt.
 * Output format: hex(salt) + "$" + hex(derived_key)
 * out_hash buffer must be at least 130 bytes.
 */
void vekd_crypto_hash_password(const char *password, char *out_hash, size_t hash_len);

/*
 * Verify a password against a stored PBKDF2-SHA256 hash.
 * Returns true if the password matches.
 */
bool vekd_crypto_verify_password(const char *password, const char *stored_hash);

/* Fill buffer with random bytes from /dev/urandom. Returns 0 on success. */
int vekd_crypto_random_bytes(uint8_t *buf, size_t len);

#endif /* VEKD_CRYPTO_H */
