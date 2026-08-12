#ifndef VEK_VALUE_H
#define VEK_VALUE_H

#include "common.h"

/*
 * NaN-Boxing Scheme (64-bit Value)
 *
 * All values are encoded in a single uint64_t using NaN-boxing.
 * IEEE 754 doubles use the quiet NaN space for tagged values.
 *
 * Encoding (top 16 bits):
 *   0x0000..0xFFFB  - IEEE 754 double (regular float), excluding our tag range
 *   0x7FF8          - Pointer (low 47 bits; 16-byte aligned, low 4 bits zero)
 *   0x7FF9          - Integer (low 48 bits, signed via sign extension)
 *   0x7FFA_0000_0000_0000 - false
 *   0x7FFA_8000_0000_0000 - true
 *   0x7FFB_0000_0000_0000 - nil
 *   0x7FFB_8000_0000_0000 - undefined
 *
 * Key insight: is_float(v) = (v & 0xFFF8000000000000) != 0x7FF8000000000000
 * Doubles require NO tag check for arithmetic in the fast path.
 */

// Tag constants
#define TAG_PTR       ((uint64_t)0x7FF8000000000000ULL)
#define TAG_INT       ((uint64_t)0x7FF9000000000000ULL)
#define VAL_FALSE     ((uint64_t)0x7FFA000000000000ULL)
#define VAL_TRUE      ((uint64_t)0x7FFA800000000000ULL)
#define VAL_NIL       ((uint64_t)0x7FFB000000000000ULL)
#define VAL_UNDEFINED ((uint64_t)0x7FFB800000000000ULL)

// Mask for top 16 bits
#define TAG_MASK      ((uint64_t)0xFFFF000000000000ULL)
// Quiet NaN bit pattern (top 13 bits that signal "this is a tagged value, not a float")
#define QNAN_BITS     ((uint64_t)0x7FF8000000000000ULL)
// Mask to extract those top 13 bits
#define QNAN_MASK     ((uint64_t)0xFFF8000000000000ULL)
// Payload mask for 48-bit values
#define PAYLOAD_MASK  ((uint64_t)0x0000FFFFFFFFFFFFULL)
// Pointer payload mask (47 bits)
#define PTR_MASK      ((uint64_t)0x00007FFFFFFFFFFFULL)

// ---- Type checks ----

// A value is a float if its top 13 bits are NOT the quiet NaN pattern 0x7FF8
// (i.e., it doesn't fall in the NaN tag space we use for non-float values)
#define IS_FLOAT(v)   (((v) & QNAN_MASK) != QNAN_BITS)

// Integer: top 16 bits are 0x7FF9
#define IS_INT(v)     (((v) & TAG_MASK) == TAG_INT)

// Pointer: top 16 bits are 0x7FF8, low 4 bits are zero (16-byte aligned)
#define IS_PTR(v)     (((v) & TAG_MASK) == TAG_PTR && (((v) & 0xF) == 0))

// Bool: exact match for true or false
#define IS_BOOL(v)    ((v) == VAL_TRUE || (v) == VAL_FALSE)

// Nil: exact match
#define IS_NIL(v)     ((v) == VAL_NIL)

// Undefined: exact match
#define IS_UNDEFINED(v) ((v) == VAL_UNDEFINED)

// ---- Decoding (AS_*) ----

// Reinterpret uint64_t as double
static inline double value_as_double(Value v) {
    double d;
    memcpy(&d, &v, sizeof(double));
    return d;
}

#define AS_DOUBLE(v)  value_as_double(v)

// Extract 48-bit signed integer via sign extension
static inline int64_t value_as_int(Value v) {
    uint64_t raw = (v) & PAYLOAD_MASK;
    // Sign-extend from bit 47
    if (raw & ((uint64_t)1 << 47)) {
        raw |= ((uint64_t)0xFFFF << 48);
    }
    return (int64_t)raw;
}

#define AS_INT(v)     value_as_int(v)

// Extract pointer (low 47 bits)
static inline void* value_as_ptr(Value v) {
    return (void*)(uintptr_t)((v) & PTR_MASK);
}

#define AS_PTR(v)     value_as_ptr(v)

// Extract boolean
#define AS_BOOL(v)    ((v) == VAL_TRUE)

// ---- Encoding (VAL_*) ----

// Encode a double as a Value (reinterpret bits)
static inline Value double_val(double d) {
    Value v;
    memcpy(&v, &d, sizeof(Value));
    return v;
}

#define FLOAT_VAL(d)  double_val(d)

// Encode a 48-bit signed integer
static inline Value int_val(int64_t i) {
    return TAG_INT | ((uint64_t)i & PAYLOAD_MASK);
}

#define INT_VAL(i)    int_val(i)

// Encode a pointer (must be 16-byte aligned)
static inline Value ptr_val(void* p) {
    return TAG_PTR | ((uint64_t)(uintptr_t)p & PTR_MASK);
}

#define PTR_VAL(p)    ptr_val(p)

// Encode booleans
#define BOOL_VAL(b)   ((b) ? VAL_TRUE : VAL_FALSE)

// Nil
#define NIL_VAL       VAL_NIL

// Undefined
#define UNDEFINED_VAL VAL_UNDEFINED

// ---- Falsiness ----
// Only nil and false are falsy (per design doc section 4.14)
#define IS_FALSY(v)   ((v) == VAL_NIL || (v) == VAL_FALSE)

// ---- Equality ----
// Two values are equal if their bit patterns are equal,
// except for doubles which use numeric comparison (handles -0.0 == 0.0, NaN != NaN)
static inline bool value_equal(Value a, Value b) {
    if (IS_FLOAT(a) && IS_FLOAT(b)) {
        return AS_DOUBLE(a) == AS_DOUBLE(b);
    }
    // For non-float values (int, bool, nil, ptr), bit-exact comparison
    return a == b;
}

#endif // VEK_VALUE_H
