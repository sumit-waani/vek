/*
 * Unit tests for the vekd database layer.
 * Tests schema creation and CRUD operations on apps/releases/users tables.
 * Uses an in-memory SQLite database (no filesystem dependency).
 */
#include "vekd_db.h"
#include "vekd_crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  test: %s ... ", #name); \
    if (test_##name()) { tests_passed++; printf("ok\n"); } \
    else { printf("FAILED\n"); } \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("\n    ASSERT FAILED: %s (line %d)\n", #cond, __LINE__); \
        return false; \
    } \
} while(0)

static VekdDB db;

/* --- Schema Tests --- */

static bool test_schema_creation(void) {
    /* Database should already be open with schema applied */
    ASSERT(db.db != NULL);

    /* Verify all tables exist by querying sqlite_master */
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db.db,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND "
        "name IN ('apps','releases','env_vars','events','secrets','users')",
        -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK);

    rc = sqlite3_step(stmt);
    ASSERT(rc == SQLITE_ROW);
    int count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    ASSERT(count == 6);
    return true;
}

/* --- App Tests --- */

static bool test_app_create(void) {
    int rc = vekd_db_app_create(&db, "myapp", "https://github.com/user/myapp.git",
                                "main", 10001);
    ASSERT(rc == 0);
    return true;
}

static bool test_app_get_by_name(void) {
    VekdApp app;
    int rc = vekd_db_app_get_by_name(&db, "myapp", &app);
    ASSERT(rc == 0);
    ASSERT(app.id == 1);
    ASSERT(strcmp(app.name, "myapp") == 0);
    ASSERT(strcmp(app.repo_url, "https://github.com/user/myapp.git") == 0);
    ASSERT(strcmp(app.branch, "main") == 0);
    ASSERT(app.port == 10001);
    ASSERT(strcmp(app.state, "pending") == 0);
    ASSERT(app.created_at > 0);
    return true;
}

static bool test_app_not_found(void) {
    VekdApp app;
    int rc = vekd_db_app_get_by_name(&db, "nonexistent", &app);
    ASSERT(rc == -1);
    return true;
}

static bool test_app_update_state(void) {
    VekdApp app;
    vekd_db_app_get_by_name(&db, "myapp", &app);
    int rc = vekd_db_app_update_state(&db, app.id, "healthy");
    ASSERT(rc == 0);

    vekd_db_app_get_by_name(&db, "myapp", &app);
    ASSERT(strcmp(app.state, "healthy") == 0);
    return true;
}

static bool test_app_count(void) {
    int count = vekd_db_app_count(&db);
    ASSERT(count == 1);

    /* Add another app */
    vekd_db_app_create(&db, "otherapp", "https://github.com/user/other.git",
                       "develop", 10002);
    count = vekd_db_app_count(&db);
    ASSERT(count == 2);
    return true;
}

static bool test_app_delete(void) {
    VekdApp app;
    vekd_db_app_get_by_name(&db, "otherapp", &app);
    int rc = vekd_db_app_delete(&db, app.id);
    ASSERT(rc == 0);

    int count = vekd_db_app_count(&db);
    ASSERT(count == 1);
    return true;
}

static bool test_app_unique_name(void) {
    /* Duplicate name should fail */
    int rc = vekd_db_app_create(&db, "myapp", "https://example.com/dup.git",
                                "main", 10003);
    ASSERT(rc == -1);
    return true;
}

/* --- Release Tests --- */

static bool test_release_create(void) {
    VekdApp app;
    vekd_db_app_get_by_name(&db, "myapp", &app);

    int rc = vekd_db_release_create(&db, app.id, "abc123def",
                                    "/var/lib/vek/apps/myapp/releases/1234/");
    ASSERT(rc == 0);
    return true;
}

/* --- Event Tests --- */

static bool test_event_log(void) {
    VekdApp app;
    vekd_db_app_get_by_name(&db, "myapp", &app);

    int rc = vekd_db_event_log(&db, app.id, "deploy", "Deployed release abc123def");
    ASSERT(rc == 0);

    rc = vekd_db_event_log(&db, app.id, "start", "App started on port 10001");
    ASSERT(rc == 0);

    /* Verify events exist */
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db.db,
        "SELECT COUNT(*) FROM events WHERE app_id = ?", -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, app.id);
    rc = sqlite3_step(stmt);
    ASSERT(rc == SQLITE_ROW);
    int count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    ASSERT(count == 2);
    return true;
}

/* --- User Tests --- */

static bool test_user_create(void) {
    int rc = vekd_db_user_create(&db, "admin@example.com",
                                 "$2b$12$hashvalue", 1);
    ASSERT(rc == 0);
    return true;
}

static bool test_user_get_by_email(void) {
    VekdUser user;
    int rc = vekd_db_user_get_by_email(&db, "admin@example.com", &user);
    ASSERT(rc == 0);
    ASSERT(user.id == 1);
    ASSERT(strcmp(user.email, "admin@example.com") == 0);
    ASSERT(strcmp(user.password_hash, "$2b$12$hashvalue") == 0);
    ASSERT(user.is_admin == 1);
    ASSERT(user.created_at > 0);
    return true;
}

static bool test_user_not_found(void) {
    VekdUser user;
    int rc = vekd_db_user_get_by_email(&db, "nobody@example.com", &user);
    ASSERT(rc == -1);
    return true;
}

static bool test_user_count(void) {
    int count = vekd_db_user_count(&db);
    ASSERT(count == 1);

    vekd_db_user_create(&db, "user2@example.com", "$2b$12$hash2", 0);
    count = vekd_db_user_count(&db);
    ASSERT(count == 2);
    return true;
}

/* --- Crypto Tests --- */

static bool test_encrypt_decrypt(void) {
    uint8_t key[VEKD_KEY_SIZE];
    memset(key, 0x42, VEKD_KEY_SIZE);

    const char *plaintext = "Hello, vekd secrets!";
    size_t len = strlen(plaintext);

    /* Output needs VEKD_NONCE_SIZE + len bytes */
    uint8_t encrypted[VEKD_NONCE_SIZE + 64];
    uint8_t decrypted[64];

    int rc = vekd_crypto_encrypt(key, (const uint8_t *)plaintext, len, encrypted);
    ASSERT(rc == 0);

    /* Encrypted (after nonce) should differ from plaintext */
    ASSERT(memcmp(encrypted + VEKD_NONCE_SIZE, plaintext, len) != 0);

    rc = vekd_crypto_decrypt(key, encrypted, VEKD_NONCE_SIZE + len, decrypted);
    ASSERT(rc == 0);

    /* Decrypted should match original */
    ASSERT(memcmp(decrypted, plaintext, len) == 0);
    return true;
}

static bool test_encrypt_large(void) {
    uint8_t key[VEKD_KEY_SIZE];
    memset(key, 0xAB, VEKD_KEY_SIZE);

    /* Test with data larger than one SHA256 block (32 bytes) */
    uint8_t data[100];
    for (int i = 0; i < 100; i++) data[i] = (uint8_t)i;

    uint8_t encrypted[VEKD_NONCE_SIZE + 100];
    uint8_t decrypted[100];

    vekd_crypto_encrypt(key, data, 100, encrypted);
    ASSERT(memcmp(encrypted + VEKD_NONCE_SIZE, data, 100) != 0);

    vekd_crypto_decrypt(key, encrypted, VEKD_NONCE_SIZE + 100, decrypted);
    ASSERT(memcmp(decrypted, data, 100) == 0);
    return true;
}

static bool test_encrypt_nonce_uniqueness(void) {
    /* Encrypting the same plaintext twice should produce different ciphertexts */
    uint8_t key[VEKD_KEY_SIZE];
    memset(key, 0x55, VEKD_KEY_SIZE);

    const char *plaintext = "duplicate";
    size_t len = strlen(plaintext);

    uint8_t enc1[VEKD_NONCE_SIZE + 64];
    uint8_t enc2[VEKD_NONCE_SIZE + 64];

    vekd_crypto_encrypt(key, (const uint8_t *)plaintext, len, enc1);
    vekd_crypto_encrypt(key, (const uint8_t *)plaintext, len, enc2);

    /* Nonces should differ */
    ASSERT(memcmp(enc1, enc2, VEKD_NONCE_SIZE) != 0);

    /* Full ciphertexts should differ */
    ASSERT(memcmp(enc1, enc2, VEKD_NONCE_SIZE + len) != 0);

    /* But both should decrypt to the same plaintext */
    uint8_t dec1[64], dec2[64];
    vekd_crypto_decrypt(key, enc1, VEKD_NONCE_SIZE + len, dec1);
    vekd_crypto_decrypt(key, enc2, VEKD_NONCE_SIZE + len, dec2);
    ASSERT(memcmp(dec1, plaintext, len) == 0);
    ASSERT(memcmp(dec2, plaintext, len) == 0);
    return true;
}

static bool test_password_hash_verify(void) {
    const char *password = "mysecretpassword123";
    char hash[128];

    vekd_crypto_hash_password(password, hash, sizeof(hash));

    /* Hash should not be empty */
    ASSERT(hash[0] != '\0');
    ASSERT(strlen(hash) == 97); /* 32 hex salt + $ + 64 hex derived */
    ASSERT(hash[32] == '$');

    /* Should verify correctly */
    ASSERT(vekd_crypto_verify_password(password, hash) == true);

    /* Wrong password should not verify */
    ASSERT(vekd_crypto_verify_password("wrongpassword", hash) == false);

    return true;
}

static bool test_password_hash_unique_salts(void) {
    const char *password = "samepassword";
    char hash1[128], hash2[128];

    vekd_crypto_hash_password(password, hash1, sizeof(hash1));
    vekd_crypto_hash_password(password, hash2, sizeof(hash2));

    /* Same password should produce different hashes (different salts) */
    ASSERT(strcmp(hash1, hash2) != 0);

    /* But both should verify */
    ASSERT(vekd_crypto_verify_password(password, hash1) == true);
    ASSERT(vekd_crypto_verify_password(password, hash2) == true);

    return true;
}

int main(void) {
    printf("=== vekd Database Tests ===\n");

    /* Open in-memory database */
    int rc = vekd_db_open(&db, ":memory:");
    if (rc != 0) {
        fprintf(stderr, "Failed to open test database\n");
        return 1;
    }

    TEST(schema_creation);
    TEST(app_create);
    TEST(app_get_by_name);
    TEST(app_not_found);
    TEST(app_update_state);
    TEST(app_count);
    TEST(app_delete);
    TEST(app_unique_name);
    TEST(release_create);
    TEST(event_log);
    TEST(user_create);
    TEST(user_get_by_email);
    TEST(user_not_found);
    TEST(user_count);
    TEST(encrypt_decrypt);
    TEST(encrypt_large);
    TEST(encrypt_nonce_uniqueness);
    TEST(password_hash_verify);
    TEST(password_hash_unique_salts);

    vekd_db_close(&db);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
