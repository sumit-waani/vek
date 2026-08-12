/*
 * Unit tests for the vek compiler (Pratt parser + bytecode generation).
 * Tests compilation of expressions, statements, and control flow.
 */

#include "common.h"
#include "chunk.h"
#include "compiler.h"
#include "object.h"
#include "memory.h"
#include "gc.h"
#include "debug.h"

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

// Helper to check if a specific opcode appears in the chunk
static bool chunk_has_opcode(Chunk* chunk, OpCode op) {
    for (int i = 0; i < chunk->count; i++) {
        if (chunk->code[i] == (uint8_t)op) return true;
    }
    return false;
}

// Helper to count how many times an opcode appears (used in some tests)
__attribute__((unused))
static int count_opcode(Chunk* chunk, OpCode op) {
    int count = 0;
    int i = 0;
    while (i < chunk->count) {
        uint8_t instr = chunk->code[i];
        if (instr == (uint8_t)op) count++;

        // Advance by instruction size
        switch (instr) {
            // 1-byte (no operand) instructions
            case OP_NIL: case OP_TRUE: case OP_FALSE:
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
            case OP_NEG: case OP_NOT:
            case OP_BAND: case OP_BOR: case OP_BXOR: case OP_BNOT:
            case OP_SHL: case OP_SHR:
            case OP_EQUAL: case OP_NOT_EQUAL:
            case OP_LESS: case OP_LESS_EQUAL: case OP_GREATER: case OP_GREATER_EQUAL:
            case OP_POP: case OP_PRINT:
            case OP_CLOSE_UPVALUE:
            case OP_GET_INDEX: case OP_SET_INDEX:
            case OP_ITER_INIT:
            case OP_RETURN:
            case OP_POWER:
                i += 1;
                break;

            // 2-byte instructions (1 byte operand)
            case OP_CALL:
            case OP_GET_UPVALUE: case OP_SET_UPVALUE:
            case OP_CONCAT:
                i += 2;
                break;

            // 3-byte instructions (2-byte operand)
            case OP_CONSTANT:
            case OP_GET_LOCAL: case OP_SET_LOCAL:
            case OP_GET_GLOBAL: case OP_SET_GLOBAL: case OP_DEFINE_GLOBAL:
            case OP_JUMP: case OP_JUMP_IF_FALSE: case OP_LOOP:
            case OP_CLOSURE:
            case OP_NEW_LIST: case OP_NEW_MAP:
            case OP_GET_FIELD: case OP_SET_FIELD:
            case OP_ITER_NEXT:
                i += 3;
                break;

            default:
                i += 1;
                break;
        }
    }
    return count;
}

// Setup and teardown for each test
static void setup(void) {
    heap_init();
    gc_init();
    intern_table_init();
}

static void teardown(void) {
    intern_table_destroy();
    gc_destroy();
    heap_destroy();
}

// ---- Tests ----

// Test: compiling a simple integer literal
static bool test_integer_literal(void) {
    setup();
    ObjFunction* fn = compile("42\n");
    ASSERT(fn != NULL);

    Chunk* chunk = &fn->chunk;
    ASSERT(chunk->count > 0);
    ASSERT(chunk_has_opcode(chunk, OP_CONSTANT));
    ASSERT(chunk_has_opcode(chunk, OP_POP));  // expression statement pops result
    ASSERT(chunk_has_opcode(chunk, OP_RETURN));

    // The constant should be 42
    ASSERT(chunk->const_count >= 1);
    ASSERT(IS_INT(chunk->constants[0]));
    ASSERT(AS_INT(chunk->constants[0]) == 42);

    teardown();
    return true;
}

// Test: compiling a float literal
static bool test_float_literal(void) {
    setup();
    ObjFunction* fn = compile("3.14\n");
    ASSERT(fn != NULL);

    Chunk* chunk = &fn->chunk;
    ASSERT(chunk->const_count >= 1);
    ASSERT(IS_FLOAT(chunk->constants[0]));
    double d = AS_DOUBLE(chunk->constants[0]);
    ASSERT(d > 3.13 && d < 3.15);

    teardown();
    return true;
}

// Test: compiling boolean and nil literals
static bool test_literals(void) {
    setup();
    ObjFunction* fn = compile("true\nfalse\nnil\n");
    ASSERT(fn != NULL);

    Chunk* chunk = &fn->chunk;
    ASSERT(chunk_has_opcode(chunk, OP_TRUE));
    ASSERT(chunk_has_opcode(chunk, OP_FALSE));
    ASSERT(chunk_has_opcode(chunk, OP_NIL));

    teardown();
    return true;
}

// Test: compiling arithmetic expressions
static bool test_arithmetic(void) {
    setup();
    ObjFunction* fn = compile("1 + 2 * 3\n");
    ASSERT(fn != NULL);

    Chunk* chunk = &fn->chunk;
    // Should have: CONST(1), CONST(2), CONST(3), MUL, ADD, POP, NIL, RETURN
    ASSERT(chunk_has_opcode(chunk, OP_ADD));
    ASSERT(chunk_has_opcode(chunk, OP_MUL));
    // Due to precedence, MUL should come before ADD
    int mul_pos = -1, add_pos = -1;
    for (int i = 0; i < chunk->count; i++) {
        if (chunk->code[i] == OP_MUL && mul_pos == -1) mul_pos = i;
        if (chunk->code[i] == OP_ADD && add_pos == -1) add_pos = i;
    }
    ASSERT(mul_pos < add_pos);  // MUL executes before ADD (correct precedence)

    teardown();
    return true;
}

// Test: compiling negation
static bool test_unary_negation(void) {
    setup();
    ObjFunction* fn = compile("-5\n");
    ASSERT(fn != NULL);

    Chunk* chunk = &fn->chunk;
    ASSERT(chunk_has_opcode(chunk, OP_CONSTANT));
    ASSERT(chunk_has_opcode(chunk, OP_NEG));

    teardown();
    return true;
}

// Test: compiling comparison operators
static bool test_comparisons(void) {
    setup();
    ObjFunction* fn = compile("1 < 2\n");
    ASSERT(fn != NULL);
    ASSERT(chunk_has_opcode(&fn->chunk, OP_LESS));

    fn = compile("1 >= 2\n");
    ASSERT(fn != NULL);
    ASSERT(chunk_has_opcode(&fn->chunk, OP_GREATER_EQUAL));

    fn = compile("1 == 2\n");
    ASSERT(fn != NULL);
    ASSERT(chunk_has_opcode(&fn->chunk, OP_EQUAL));

    fn = compile("1 != 2\n");
    ASSERT(fn != NULL);
    ASSERT(chunk_has_opcode(&fn->chunk, OP_NOT_EQUAL));

    teardown();
    return true;
}

// Test: string literal compilation
static bool test_string_literal(void) {
    setup();
    ObjFunction* fn = compile("\"hello\"\n");
    ASSERT(fn != NULL);

    Chunk* chunk = &fn->chunk;
    ASSERT(chunk->const_count >= 1);
    Value v = chunk->constants[0];
    ASSERT(IS_PTR(v));
    ObjString* str = AS_STRING(v);
    ASSERT(str->length == 5);
    ASSERT(memcmp(str->data, "hello", 5) == 0);

    teardown();
    return true;
}

// Test: global variable declaration via assignment
static bool test_global_variable(void) {
    setup();
    ObjFunction* fn = compile("x = 10\nx\n");
    ASSERT(fn != NULL);

    Chunk* chunk = &fn->chunk;
    // Should produce: CONSTANT(10), DEFINE_GLOBAL("x"), GET_GLOBAL("x"), POP
    ASSERT(chunk_has_opcode(chunk, OP_DEFINE_GLOBAL));
    ASSERT(chunk_has_opcode(chunk, OP_GET_GLOBAL));

    teardown();
    return true;
}

// Test: if/else produces correct jump structure
static bool test_if_else(void) {
    setup();
    ObjFunction* fn = compile("if true\n  1\nelse\n  2\nend\n");
    ASSERT(fn != NULL);

    Chunk* chunk = &fn->chunk;
    ASSERT(chunk_has_opcode(chunk, OP_JUMP_IF_FALSE));
    ASSERT(chunk_has_opcode(chunk, OP_JUMP));

    teardown();
    return true;
}

// Test: while loop produces correct back-jump
static bool test_while_loop(void) {
    setup();
    ObjFunction* fn = compile("while true\n  1\nend\n");
    ASSERT(fn != NULL);

    Chunk* chunk = &fn->chunk;
    ASSERT(chunk_has_opcode(chunk, OP_JUMP_IF_FALSE));
    ASSERT(chunk_has_opcode(chunk, OP_LOOP));

    teardown();
    return true;
}

// Test: function declaration produces closure
static bool test_function_declaration(void) {
    setup();
    ObjFunction* fn = compile("fn add(a, b)\n  a + b\nend\n");
    ASSERT(fn != NULL);

    Chunk* chunk = &fn->chunk;
    ASSERT(chunk_has_opcode(chunk, OP_CLOSURE));
    ASSERT(chunk_has_opcode(chunk, OP_DEFINE_GLOBAL));

    // The function constant should be an ObjFunction
    bool found_func = false;
    for (int i = 0; i < chunk->const_count; i++) {
        if (IS_PTR(chunk->constants[i])) {
            ObjHeader* obj = (ObjHeader*)AS_PTR(chunk->constants[i]);
            if (obj->type == OBJ_FUNCTION) {
                ObjFunction* inner = (ObjFunction*)obj;
                ASSERT(inner->arity == 2);
                found_func = true;
                break;
            }
        }
    }
    ASSERT(found_func);

    teardown();
    return true;
}

// Test: local variables in function
static bool test_local_variables(void) {
    setup();
    ObjFunction* fn = compile("fn test()\n  x = 5\n  x\nend\n");
    ASSERT(fn != NULL);

    // The inner function should use SET_LOCAL and GET_LOCAL
    for (int i = 0; i < fn->chunk.const_count; i++) {
        if (IS_PTR(fn->chunk.constants[i])) {
            ObjHeader* obj = (ObjHeader*)AS_PTR(fn->chunk.constants[i]);
            if (obj->type == OBJ_FUNCTION) {
                ObjFunction* inner = (ObjFunction*)obj;
                ASSERT(chunk_has_opcode(&inner->chunk, OP_SET_LOCAL));
                ASSERT(chunk_has_opcode(&inner->chunk, OP_GET_LOCAL));
                break;
            }
        }
    }

    teardown();
    return true;
}

// Test: list literal
static bool test_list_literal(void) {
    setup();
    ObjFunction* fn = compile("[1, 2, 3]\n");
    ASSERT(fn != NULL);
    ASSERT(chunk_has_opcode(&fn->chunk, OP_NEW_LIST));

    teardown();
    return true;
}

// Test: map literal
static bool test_map_literal(void) {
    setup();
    ObjFunction* fn = compile("{a: 1, b: 2}\n");
    ASSERT(fn != NULL);
    ASSERT(chunk_has_opcode(&fn->chunk, OP_NEW_MAP));

    teardown();
    return true;
}

// Test: compiler reports errors for bad syntax
static bool test_syntax_error(void) {
    setup();
    ObjFunction* fn = compile("1 +\n");
    // Should fail to compile (incomplete expression)
    ASSERT(fn == NULL);

    teardown();
    return true;
}

// Test: nested scopes work correctly
static bool test_nested_scopes(void) {
    setup();
    const char* source =
        "fn outer()\n"
        "  x = 1\n"
        "  fn inner()\n"
        "    x\n"
        "  end\n"
        "  inner\n"
        "end\n";
    ObjFunction* fn = compile(source);
    ASSERT(fn != NULL);

    teardown();
    return true;
}

// Test: logical operators (&&, ||) produce short-circuit jumps
static bool test_logical_operators(void) {
    setup();
    ObjFunction* fn = compile("true && false\n");
    ASSERT(fn != NULL);
    ASSERT(chunk_has_opcode(&fn->chunk, OP_JUMP_IF_FALSE));

    fn = compile("true || false\n");
    ASSERT(fn != NULL);
    ASSERT(chunk_has_opcode(&fn->chunk, OP_JUMP_IF_FALSE));
    ASSERT(chunk_has_opcode(&fn->chunk, OP_JUMP));

    teardown();
    return true;
}

// Test: disassembler runs without crashing
static bool test_disassembler(void) {
    setup();
    ObjFunction* fn = compile("1 + 2\n");
    ASSERT(fn != NULL);

    // Just call disassemble and verify it doesn't crash
    printf("\n");
    disassemble_chunk(&fn->chunk, "test");

    teardown();
    return true;
}

// Test: line numbers are tracked
static bool test_line_tracking(void) {
    setup();
    ObjFunction* fn = compile("1\n2\n3\n");
    ASSERT(fn != NULL);

    Chunk* chunk = &fn->chunk;
    // Lines array should be populated
    ASSERT(chunk->line_count > 0);
    // First instruction should be on line 1
    ASSERT(chunk->lines[0] == 1);

    teardown();
    return true;
}

// Test: break in a loop
static bool test_break_in_loop(void) {
    setup();
    ObjFunction* fn = compile("while true\n  break\nend\n");
    ASSERT(fn != NULL);

    Chunk* chunk = &fn->chunk;
    ASSERT(chunk_has_opcode(chunk, OP_JUMP));  // break uses JUMP
    ASSERT(chunk_has_opcode(chunk, OP_LOOP));  // loop back

    teardown();
    return true;
}

// ---- Main ----

int main(void) {
    printf("=== test_compiler ===\n");

    TEST(integer_literal);
    TEST(float_literal);
    TEST(literals);
    TEST(arithmetic);
    TEST(unary_negation);
    TEST(comparisons);
    TEST(string_literal);
    TEST(global_variable);
    TEST(if_else);
    TEST(while_loop);
    TEST(function_declaration);
    TEST(local_variables);
    TEST(list_literal);
    TEST(map_literal);
    TEST(syntax_error);
    TEST(nested_scopes);
    TEST(logical_operators);
    TEST(disassembler);
    TEST(line_tracking);
    TEST(break_in_loop);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
