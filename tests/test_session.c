/*
 * Unit tests for the session stdlib package.
 * Tests HMAC-SHA256, base64 encode/decode, session encode/decode.
 */
#include "common.h"
#include "value.h"
#include "memory.h"
#include "gc.h"
#include "object.h"
#include "vm.h"
#include "vek_stdlib.h"

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

// External declarations from stdlib_session.c
extern void hmac_sha256(const uint8_t* key, size_t key_len,
                        const uint8_t* msg, size_t msg_len,
                        uint8_t out[32]);
extern char* base64_encode(const uint8_t* data, size_t len, size_t* out_len);
extern uint8_t* base64_decode(const char* data, size_t len, size_t* out_len);

// Helper: call a native function registered in a package
static Value call_session_fn(const char* name, int argc, Value* args) {
    ObjString* pkg_key = obj_string_new("session", 7);
    Value pkg_val;
    if (!obj_map_get(vm.globals, pkg_key, &pkg_val)) return VAL_NIL;
    ObjMap* pkg = AS_MAP(pkg_val);

    ObjString* fn_key = obj_string_new(name, (uint32_t)strlen(name));
    Value fn_val;
    if (!obj_map_get(pkg, fn_key, &fn_val)) return VAL_NIL;
    ObjNative* native = AS_NATIVE(fn_val);

    return native->function(argc, args);
}

// ---- HMAC-SHA256 Tests ----

static bool test_hmac_sha256_rfc4231_1(void) {
    // RFC 4231 Test Case 1
    // Key = 0x0b repeated 20 times
    // Data = "Hi There"
    // HMAC-SHA256 = b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7
    uint8_t key[20];
    memset(key, 0x0b, 20);
    const char* data = "Hi There";
    uint8_t result[32];

    hmac_sha256(key, 20, (const uint8_t*)data, 8, result);

    // Check against expected
    uint8_t expected[32] = {
        0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53,
        0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
        0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7,
        0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7
    };

    ASSERT(memcmp(result, expected, 32) == 0);
    return true;
}

static bool test_hmac_sha256_rfc4231_2(void) {
    // RFC 4231 Test Case 2
    // Key = "Jefe"
    // Data = "what do ya want for nothing?"
    // HMAC-SHA256 = 5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843
    const char* key = "Jefe";
    const char* data = "what do ya want for nothing?";
    uint8_t result[32];

    hmac_sha256((const uint8_t*)key, 4, (const uint8_t*)data, 28, result);

    uint8_t expected[32] = {
        0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e,
        0x6a, 0x04, 0x24, 0x26, 0x08, 0x95, 0x75, 0xc7,
        0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83,
        0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43
    };

    ASSERT(memcmp(result, expected, 32) == 0);
    return true;
}

static bool test_hmac_sha256_long_key(void) {
    // RFC 4231 Test Case 3
    // Key = 0xaa repeated 20 times
    // Data = 0xdd repeated 50 times
    // HMAC-SHA256 = 773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe
    uint8_t key[20];
    memset(key, 0xaa, 20);
    uint8_t data[50];
    memset(data, 0xdd, 50);
    uint8_t result[32];

    hmac_sha256(key, 20, data, 50, result);

    uint8_t expected[32] = {
        0x77, 0x3e, 0xa9, 0x1e, 0x36, 0x80, 0x0e, 0x46,
        0x85, 0x4d, 0xb8, 0xeb, 0xd0, 0x91, 0x81, 0xa7,
        0x29, 0x59, 0x09, 0x8b, 0x3e, 0xf8, 0xc1, 0x22,
        0xd9, 0x63, 0x55, 0x14, 0xce, 0xd5, 0x65, 0xfe
    };

    ASSERT(memcmp(result, expected, 32) == 0);
    return true;
}

// ---- Base64 Tests ----

static bool test_base64_encode_empty(void) {
    size_t out_len = 0;
    char* result = base64_encode((const uint8_t*)"", 0, &out_len);
    ASSERT(result != NULL);
    ASSERT(out_len == 0);
    ASSERT(result[0] == '\0');
    free(result);
    return true;
}

static bool test_base64_encode_hello(void) {
    size_t out_len = 0;
    char* result = base64_encode((const uint8_t*)"Hello", 5, &out_len);
    ASSERT(result != NULL);
    ASSERT(strcmp(result, "SGVsbG8=") == 0);
    free(result);
    return true;
}

static bool test_base64_encode_padding(void) {
    // "Ma" -> "TWE=" (1 padding)
    // "M"  -> "TQ==" (2 padding)
    size_t out_len = 0;

    char* r1 = base64_encode((const uint8_t*)"Ma", 2, &out_len);
    ASSERT(strcmp(r1, "TWE=") == 0);
    free(r1);

    char* r2 = base64_encode((const uint8_t*)"M", 1, &out_len);
    ASSERT(strcmp(r2, "TQ==") == 0);
    free(r2);

    // No padding: "Man" -> "TWFu"
    char* r3 = base64_encode((const uint8_t*)"Man", 3, &out_len);
    ASSERT(strcmp(r3, "TWFu") == 0);
    free(r3);

    return true;
}

static bool test_base64_decode_hello(void) {
    size_t out_len = 0;
    uint8_t* result = base64_decode("SGVsbG8=", 8, &out_len);
    ASSERT(result != NULL);
    ASSERT(out_len == 5);
    ASSERT(memcmp(result, "Hello", 5) == 0);
    free(result);
    return true;
}

static bool test_base64_roundtrip(void) {
    const char* input = "The quick brown fox jumps over the lazy dog";
    size_t input_len = strlen(input);

    size_t enc_len = 0;
    char* encoded = base64_encode((const uint8_t*)input, input_len, &enc_len);
    ASSERT(encoded != NULL);

    size_t dec_len = 0;
    uint8_t* decoded = base64_decode(encoded, enc_len, &dec_len);
    ASSERT(decoded != NULL);
    ASSERT(dec_len == input_len);
    ASSERT(memcmp(decoded, input, input_len) == 0);

    free(encoded);
    free(decoded);
    return true;
}

static bool test_base64_binary_roundtrip(void) {
    // Test with binary data including zeros
    uint8_t data[256];
    for (int i = 0; i < 256; i++) data[i] = (uint8_t)i;

    size_t enc_len = 0;
    char* encoded = base64_encode(data, 256, &enc_len);
    ASSERT(encoded != NULL);

    size_t dec_len = 0;
    uint8_t* decoded = base64_decode(encoded, enc_len, &dec_len);
    ASSERT(decoded != NULL);
    ASSERT(dec_len == 256);
    ASSERT(memcmp(decoded, data, 256) == 0);

    free(encoded);
    free(decoded);
    return true;
}

// ---- Session encode/decode Tests ----

static bool test_session_init(void) {
    Value args[1];
    args[0] = OBJ_VAL(obj_string_new("my-secret-key", 13));
    Value result = call_session_fn("init", 1, args);
    ASSERT(result == VAL_TRUE);
    return true;
}

static bool test_session_set_get(void) {
    // Set a value
    Value set_args[2];
    set_args[0] = OBJ_VAL(obj_string_new("user", 4));
    set_args[1] = OBJ_VAL(obj_string_new("alice", 5));
    Value set_result = call_session_fn("set", 2, set_args);
    ASSERT(set_result == VAL_TRUE);

    // Get it back
    Value get_args[1];
    get_args[0] = OBJ_VAL(obj_string_new("user", 4));
    Value get_result = call_session_fn("get", 1, get_args);
    ASSERT(IS_STRING(get_result));
    ASSERT(strcmp(AS_STRING(get_result)->data, "alice") == 0);
    return true;
}

static bool test_session_get_missing(void) {
    Value get_args[1];
    get_args[0] = OBJ_VAL(obj_string_new("nonexistent", 11));
    Value result = call_session_fn("get", 1, get_args);
    ASSERT(IS_NIL(result));
    return true;
}

static bool test_session_delete(void) {
    // Set then delete
    Value set_args[2];
    set_args[0] = OBJ_VAL(obj_string_new("temp", 4));
    set_args[1] = INT_VAL(42);
    call_session_fn("set", 2, set_args);

    Value del_args[1];
    del_args[0] = OBJ_VAL(obj_string_new("temp", 4));
    call_session_fn("delete", 1, del_args);

    // Should be nil now
    Value get_args[1];
    get_args[0] = OBJ_VAL(obj_string_new("temp", 4));
    Value result = call_session_fn("get", 1, get_args);
    ASSERT(IS_NIL(result));
    return true;
}

static bool test_session_encode_decode(void) {
    // Set some data
    Value set_args[2];
    set_args[0] = OBJ_VAL(obj_string_new("name", 4));
    set_args[1] = OBJ_VAL(obj_string_new("bob", 3));
    call_session_fn("set", 2, set_args);

    // Encode
    Value cookie = call_session_fn("encode", 0, NULL);
    ASSERT(IS_STRING(cookie));

    // Cookie should contain a dot separator
    ObjString* cookie_str = AS_STRING(cookie);
    bool has_dot = false;
    for (uint32_t i = 0; i < cookie_str->length; i++) {
        if (cookie_str->data[i] == '.') { has_dot = true; break; }
    }
    ASSERT(has_dot);

    // Decode it back (this replaces session data)
    Value decode_args[1];
    decode_args[0] = cookie;
    Value decode_result = call_session_fn("decode", 1, decode_args);
    ASSERT(decode_result == VAL_TRUE);

    // Verify data is still there
    Value get_args[1];
    get_args[0] = OBJ_VAL(obj_string_new("name", 4));
    Value result = call_session_fn("get", 1, get_args);
    ASSERT(IS_STRING(result));
    ASSERT(strcmp(AS_STRING(result)->data, "bob") == 0);
    return true;
}

static bool test_session_decode_tampered(void) {
    // Encode valid session
    Value set_args[2];
    set_args[0] = OBJ_VAL(obj_string_new("key", 3));
    set_args[1] = OBJ_VAL(obj_string_new("val", 3));
    call_session_fn("set", 2, set_args);

    Value cookie = call_session_fn("encode", 0, NULL);
    ASSERT(IS_STRING(cookie));

    // Tamper with the cookie by modifying a character
    ObjString* orig = AS_STRING(cookie);
    char* tampered = (char*)malloc(orig->length + 1);
    memcpy(tampered, orig->data, orig->length);
    tampered[orig->length] = '\0';
    // Flip a character in the data portion
    if (tampered[0] == 'A') tampered[0] = 'B';
    else tampered[0] = 'A';

    ObjString* tampered_str = obj_string_new(tampered, orig->length);
    free(tampered);

    Value decode_args[1];
    decode_args[0] = OBJ_VAL(tampered_str);
    Value result = call_session_fn("decode", 1, decode_args);
    // Should fail (nil) because signature doesn't match
    ASSERT(IS_NIL(result));
    return true;
}

static bool test_session_flash(void) {
    // Set a flash message
    Value set_args[2];
    set_args[0] = OBJ_VAL(obj_string_new("notice", 6));
    set_args[1] = OBJ_VAL(obj_string_new("Saved!", 6));
    call_session_fn("flash_set", 2, set_args);

    // Get it (should return and remove)
    Value get_args[1];
    get_args[0] = OBJ_VAL(obj_string_new("notice", 6));
    Value result = call_session_fn("flash_get", 1, get_args);
    ASSERT(IS_STRING(result));
    ASSERT(strcmp(AS_STRING(result)->data, "Saved!") == 0);

    // Getting again should return nil
    Value result2 = call_session_fn("flash_get", 1, get_args);
    ASSERT(IS_NIL(result2));
    return true;
}

int main(void) {
    gc_init();
    intern_table_init();
    heap_init();
    vm_init();
    stdlib_init();

    printf("=== Session Tests ===\n");

    // HMAC tests
    TEST(hmac_sha256_rfc4231_1);
    TEST(hmac_sha256_rfc4231_2);
    TEST(hmac_sha256_long_key);

    // Base64 tests
    TEST(base64_encode_empty);
    TEST(base64_encode_hello);
    TEST(base64_encode_padding);
    TEST(base64_decode_hello);
    TEST(base64_roundtrip);
    TEST(base64_binary_roundtrip);

    // Session tests
    TEST(session_init);
    TEST(session_set_get);
    TEST(session_get_missing);
    TEST(session_delete);
    TEST(session_encode_decode);
    TEST(session_decode_tampered);
    TEST(session_flash);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);

    vm_free();
    intern_table_destroy();
    gc_destroy();
    heap_destroy();

    return tests_passed == tests_run ? 0 : 1;
}
