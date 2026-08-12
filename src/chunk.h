#ifndef VEK_CHUNK_H
#define VEK_CHUNK_H

#include "common.h"
#include "value.h"

// Opcodes - stack-based for Phase 1 (simplified from design doc's register-based spec)
// The VM in Phase 1 uses a stack discipline: locals accessed by slot index,
// temporaries on the stack top.
typedef enum {
    // Constants and literals
    OP_CONSTANT,        // [const_idx:16] push constants[idx]
    OP_NIL,             // push nil
    OP_TRUE,            // push true
    OP_FALSE,           // push false

    // Arithmetic
    OP_ADD,             // pop b, pop a, push a + b
    OP_SUB,             // pop b, pop a, push a - b
    OP_MUL,             // pop b, pop a, push a * b
    OP_DIV,             // pop b, pop a, push a / b (always float)
    OP_MOD,             // pop b, pop a, push a % b
    OP_NEG,             // pop a, push -a
    OP_NOT,             // pop a, push !a (truthy inversion)

    // Bitwise
    OP_BAND,            // pop b, pop a, push a & b
    OP_BOR,             // pop b, pop a, push a | b
    OP_BXOR,            // pop b, pop a, push a ^ b
    OP_BNOT,            // pop a, push ~a
    OP_SHL,             // pop b, pop a, push a << b
    OP_SHR,             // pop b, pop a, push a >> b

    // Comparison
    OP_EQUAL,           // pop b, pop a, push a == b
    OP_NOT_EQUAL,       // pop b, pop a, push a != b
    OP_LESS,            // pop b, pop a, push a < b
    OP_LESS_EQUAL,      // pop b, pop a, push a <= b
    OP_GREATER,         // pop b, pop a, push a > b
    OP_GREATER_EQUAL,   // pop b, pop a, push a >= b

    // String
    OP_CONCAT,          // [count:8] pop count strings, push concatenation

    // Stack manipulation
    OP_POP,             // pop and discard top
    OP_PRINT,           // pop and print (temporary built-in for testing)

    // Variables
    OP_GET_LOCAL,       // [slot:16] push locals[slot]
    OP_SET_LOCAL,       // [slot:16] locals[slot] = peek(0)
    OP_GET_GLOBAL,      // [name_idx:16] push globals[name]
    OP_SET_GLOBAL,      // [name_idx:16] globals[name] = peek(0)
    OP_DEFINE_GLOBAL,   // [name_idx:16] globals[name] = pop()

    // Upvalues
    OP_GET_UPVALUE,     // [slot:8] push upvalues[slot]
    OP_SET_UPVALUE,     // [slot:8] upvalues[slot] = peek(0)
    OP_CLOSE_UPVALUE,   // close the upvalue on top of the stack

    // Control flow
    OP_JUMP,            // [offset:16] ip += offset (unconditional forward)
    OP_JUMP_IF_FALSE,   // [offset:16] if falsy(peek(0)), ip += offset; pop
    OP_LOOP,            // [offset:16] ip -= offset (backward jump)

    // Functions
    OP_CALL,            // [arg_count:8] call function with arg_count arguments
    OP_RETURN,          // return top of stack from current function
    OP_CLOSURE,         // [const_idx:16] create closure from function constant
                        // followed by upvalue data: pairs of (is_local:8, index:8)

    // Collections
    OP_NEW_LIST,        // [count:16] pop count values, push list
    OP_NEW_MAP,         // [count:16] pop 2*count values (key,val pairs), push map

    // Index/field access
    OP_GET_INDEX,       // pop index, pop object, push object[index]
    OP_SET_INDEX,       // pop value, pop index, pop object, object[index] = value
    OP_GET_FIELD,       // [name_idx:16] pop object, push object.field
    OP_SET_FIELD,       // [name_idx:16] pop value, pop object, object.field = value

    // Iteration
    OP_ITER_INIT,       // pop iterable, push iterator
    OP_ITER_NEXT,       // [offset:16] advance iterator, push value or jump if done

    // Exponent
    OP_POWER,           // pop b, pop a, push a ** b

    OP_COUNT            // total number of opcodes
} OpCode;

// Bytecode chunk: a dynamic array of bytes + constants + line info
typedef struct Chunk {
    uint8_t* code;          // bytecode array
    int      count;         // number of bytes written
    int      capacity;      // allocated capacity

    Value*   constants;     // constant pool
    int      const_count;   // number of constants
    int      const_capacity;// constant pool capacity

    int*     lines;         // source line per byte (run-length would be better but simple for now)
    int      line_count;    // should equal count
    int      line_capacity; // allocated capacity
} Chunk;

// Initialize a chunk to empty
void chunk_init(Chunk* chunk);

// Write a byte to the chunk
void chunk_write(Chunk* chunk, uint8_t byte, int line);

// Add a constant to the chunk's constant pool, return its index
int chunk_add_constant(Chunk* chunk, Value value);

// Free all memory owned by the chunk
void chunk_free(Chunk* chunk);

#endif // VEK_CHUNK_H
