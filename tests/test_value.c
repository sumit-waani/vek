/*
 * Unit tests for NaN-boxing value representation.
 * Tests encoding roundtrips for all value types.
 */
#define _POSIX_C_SOURCE 200112L
#include "common.h"
#include "value.h"
#include <math.h>

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

// ---- Integer tests ----

static bool test_int_zero(void) {
    Value v = INT_VAL(0);
    ASSERT(IS_INT(v));
    ASSERT(!IS_FLOAT(v));
    ASSERT(!IS_BOOL(v));
    ASSERT(!IS_NIL(v));
    ASSERT(!IS_PTR(v));
    ASSERT(AS_INT(v) == 0);
    return true;
}

static bool test_int_positive(void) {
    Value v = INT_VAL(42);
    ASSERT(IS_INT(v));
    ASSERT(AS_INT(v) == 42);
    return true;
}

static bool test_int_negative(void) {
    Value v = INT_VAL(-1);
    ASSERT(IS_INT(v));
    ASSERT(AS_INT(v) == -1);
    return true;
}

static bool test_int_large_positive(void) {
    int64_t big = ((int64_t)1 << 47) - 1;  // max 48-bit signed
    Value v = INT_VAL(big);
    ASSERT(IS_INT(v));
    ASSERT(AS_INT(v) == big);
    return true;
}

static bool test_int_large_negative(void) {
    int64_t neg = -((int64_t)1 << 47);  // min 48-bit signed
    Value v = INT_VAL(neg);
    ASSERT(IS_INT(v));
    ASSERT(AS_INT(v) == neg);
    return true;
}

// ---- Float tests ----

static bool test_float_zero(void) {
    Value v = FLOAT_VAL(0.0);
    ASSERT(IS_FLOAT(v));
    ASSERT(!IS_INT(v));
    ASSERT(!IS_BOOL(v));
    ASSERT(!IS_NIL(v));
    ASSERT(AS_DOUBLE(v) == 0.0);
    return true;
}

static bool test_float_pi(void) {
    Value v = FLOAT_VAL(3.14159265358979);
    ASSERT(IS_FLOAT(v));
    ASSERT(AS_DOUBLE(v) == 3.14159265358979);
    return true;
}

static bool test_float_negative(void) {
    Value v = FLOAT_VAL(-273.15);
    ASSERT(IS_FLOAT(v));
    ASSERT(AS_DOUBLE(v) == -273.15);
    return true;
}

static bool test_float_infinity(void) {
    Value v = FLOAT_VAL(INFINITY);
    ASSERT(IS_FLOAT(v));
    ASSERT(isinf(AS_DOUBLE(v)));
    return true;
}

static bool test_float_neg_infinity(void) {
    Value v = FLOAT_VAL(-INFINITY);
    ASSERT(IS_FLOAT(v));
    ASSERT(isinf(AS_DOUBLE(v)));
    ASSERT(AS_DOUBLE(v) < 0);
    return true;
}

// ---- Bool tests ----

static bool test_bool_true(void) {
    Value v = BOOL_VAL(true);
    ASSERT(IS_BOOL(v));
    ASSERT(AS_BOOL(v) == true);
    ASSERT(!IS_INT(v));
    ASSERT(!IS_FLOAT(v));
    ASSERT(!IS_NIL(v));
    return true;
}

static bool test_bool_false(void) {
    Value v = BOOL_VAL(false);
    ASSERT(IS_BOOL(v));
    ASSERT(AS_BOOL(v) == false);
    return true;
}

// ---- Nil tests ----

static bool test_nil(void) {
    Value v = NIL_VAL;
    ASSERT(IS_NIL(v));
    ASSERT(!IS_BOOL(v));
    ASSERT(!IS_INT(v));
    ASSERT(!IS_FLOAT(v));
    ASSERT(!IS_PTR(v));
    return true;
}

// ---- Pointer tests ----

static bool test_ptr_roundtrip(void) {
    // Allocate 16-byte aligned memory
    void* p = NULL;
    int ret = posix_memalign(&p, 16, 64);
    ASSERT(ret == 0 && p != NULL);

    Value v = PTR_VAL(p);
    ASSERT(IS_PTR(v));
    ASSERT(!IS_FLOAT(v));
    ASSERT(!IS_INT(v));
    ASSERT(AS_PTR(v) == p);

    free(p);
    return true;
}

static bool test_ptr_null(void) {
    Value v = PTR_VAL(NULL);
    // NULL pointer has low bits zero so IS_PTR should be true
    ASSERT(IS_PTR(v));
    ASSERT(AS_PTR(v) == NULL);
    return true;
}

// ---- Falsiness tests ----

static bool test_falsy_nil(void) {
    ASSERT(IS_FALSY(NIL_VAL));
    return true;
}

static bool test_falsy_false(void) {
    ASSERT(IS_FALSY(BOOL_VAL(false)));
    return true;
}

static bool test_truthy_true(void) {
    ASSERT(!IS_FALSY(BOOL_VAL(true)));
    return true;
}

static bool test_truthy_zero_int(void) {
    // 0 is truthy in vek (only nil and false are falsy)
    ASSERT(!IS_FALSY(INT_VAL(0)));
    return true;
}

static bool test_truthy_zero_float(void) {
    ASSERT(!IS_FALSY(FLOAT_VAL(0.0)));
    return true;
}

// ---- Equality tests ----

static bool test_equal_ints(void) {
    ASSERT(value_equal(INT_VAL(42), INT_VAL(42)));
    ASSERT(!value_equal(INT_VAL(42), INT_VAL(43)));
    return true;
}

static bool test_equal_floats(void) {
    ASSERT(value_equal(FLOAT_VAL(3.14), FLOAT_VAL(3.14)));
    ASSERT(!value_equal(FLOAT_VAL(3.14), FLOAT_VAL(2.71)));
    return true;
}

static bool test_equal_bools(void) {
    ASSERT(value_equal(BOOL_VAL(true), BOOL_VAL(true)));
    ASSERT(!value_equal(BOOL_VAL(true), BOOL_VAL(false)));
    return true;
}

static bool test_equal_nil(void) {
    ASSERT(value_equal(NIL_VAL, NIL_VAL));
    ASSERT(!value_equal(NIL_VAL, BOOL_VAL(false)));
    return true;
}

int main(void) {
    printf("=== Value NaN-Boxing Tests ===\n");

    TEST(int_zero);
    TEST(int_positive);
    TEST(int_negative);
    TEST(int_large_positive);
    TEST(int_large_negative);

    TEST(float_zero);
    TEST(float_pi);
    TEST(float_negative);
    TEST(float_infinity);
    TEST(float_neg_infinity);

    TEST(bool_true);
    TEST(bool_false);

    TEST(nil);

    TEST(ptr_roundtrip);
    TEST(ptr_null);

    TEST(falsy_nil);
    TEST(falsy_false);
    TEST(truthy_true);
    TEST(truthy_zero_int);
    TEST(truthy_zero_float);

    TEST(equal_ints);
    TEST(equal_floats);
    TEST(equal_bools);
    TEST(equal_nil);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
