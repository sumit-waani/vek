#include "debug.h"
#include "object.h"
#include "value.h"

static void print_value(Value value) {
    if (IS_INT(value)) {
        printf("%lld", (long long)AS_INT(value));
    } else if (IS_FLOAT(value)) {
        printf("%g", AS_DOUBLE(value));
    } else if (IS_BOOL(value)) {
        printf("%s", AS_BOOL(value) ? "true" : "false");
    } else if (IS_NIL(value)) {
        printf("nil");
    } else if (IS_PTR(value)) {
        ObjHeader* obj = (ObjHeader*)AS_PTR(value);
        if (obj->type == OBJ_STRING) {
            ObjString* str = (ObjString*)obj;
            printf("\"%.*s\"", (int)str->length, str->data);
        } else if (obj->type == OBJ_FUNCTION) {
            ObjFunction* func = (ObjFunction*)obj;
            if (func->name) {
                printf("<fn %.*s>", (int)func->name->length, func->name->data);
            } else {
                printf("<script>");
            }
        } else {
            printf("<obj %d>", obj->type);
        }
    } else {
        printf("???");
    }
}

// Simple instruction (no operands, 1 byte)
static int simple_instruction(const char* name, int offset) {
    printf("%s\n", name);
    return offset + 1;
}

// Instruction with a 16-bit operand (constant index, local slot, etc.)
static int constant_instruction(const char* name, Chunk* chunk, int offset) {
    uint16_t idx = (uint16_t)(chunk->code[offset + 1] | (chunk->code[offset + 2] << 8));
    printf("%-20s %5d '", name, idx);
    if (idx < (uint16_t)chunk->const_count) {
        print_value(chunk->constants[idx]);
    } else {
        printf("???");
    }
    printf("'\n");
    return offset + 3;
}

// Instruction with a single byte operand
static int byte_instruction(const char* name, Chunk* chunk, int offset) {
    uint8_t slot = chunk->code[offset + 1];
    printf("%-20s %5d\n", name, slot);
    return offset + 2;
}

// Instruction with a 16-bit slot operand (locals, globals)
static int short_instruction(const char* name, Chunk* chunk, int offset) {
    uint16_t slot = (uint16_t)(chunk->code[offset + 1] | (chunk->code[offset + 2] << 8));
    printf("%-20s %5d\n", name, slot);
    return offset + 3;
}

// Jump instruction with a 16-bit offset
static int jump_instruction(const char* name, int sign, Chunk* chunk, int offset) {
    uint16_t jump = (uint16_t)(chunk->code[offset + 1] | (chunk->code[offset + 2] << 8));
    printf("%-20s %5d -> %d\n", name, offset, offset + 3 + sign * jump);
    return offset + 3;
}

void disassemble_chunk(Chunk* chunk, const char* name) {
    printf("== %s ==\n", name);
    for (int offset = 0; offset < chunk->count; ) {
        offset = disassemble_instruction(chunk, offset);
    }
}

int disassemble_instruction(Chunk* chunk, int offset) {
    printf("%04d ", offset);

    // Print line info
    if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
        printf("   | ");
    } else {
        printf("%4d ", chunk->lines[offset]);
    }

    uint8_t instruction = chunk->code[offset];
    switch (instruction) {
        case OP_CONSTANT:
            return constant_instruction("OP_CONSTANT", chunk, offset);
        case OP_NIL:
            return simple_instruction("OP_NIL", offset);
        case OP_TRUE:
            return simple_instruction("OP_TRUE", offset);
        case OP_FALSE:
            return simple_instruction("OP_FALSE", offset);

        case OP_ADD:
            return simple_instruction("OP_ADD", offset);
        case OP_SUB:
            return simple_instruction("OP_SUB", offset);
        case OP_MUL:
            return simple_instruction("OP_MUL", offset);
        case OP_DIV:
            return simple_instruction("OP_DIV", offset);
        case OP_MOD:
            return simple_instruction("OP_MOD", offset);
        case OP_NEG:
            return simple_instruction("OP_NEG", offset);
        case OP_NOT:
            return simple_instruction("OP_NOT", offset);

        case OP_BAND:
            return simple_instruction("OP_BAND", offset);
        case OP_BOR:
            return simple_instruction("OP_BOR", offset);
        case OP_BXOR:
            return simple_instruction("OP_BXOR", offset);
        case OP_BNOT:
            return simple_instruction("OP_BNOT", offset);
        case OP_SHL:
            return simple_instruction("OP_SHL", offset);
        case OP_SHR:
            return simple_instruction("OP_SHR", offset);

        case OP_EQUAL:
            return simple_instruction("OP_EQUAL", offset);
        case OP_NOT_EQUAL:
            return simple_instruction("OP_NOT_EQUAL", offset);
        case OP_LESS:
            return simple_instruction("OP_LESS", offset);
        case OP_LESS_EQUAL:
            return simple_instruction("OP_LESS_EQUAL", offset);
        case OP_GREATER:
            return simple_instruction("OP_GREATER", offset);
        case OP_GREATER_EQUAL:
            return simple_instruction("OP_GREATER_EQUAL", offset);

        case OP_CONCAT: {
            uint8_t count = chunk->code[offset + 1];
            printf("%-20s %5d\n", "OP_CONCAT", count);
            return offset + 2;
        }

        case OP_POP:
            return simple_instruction("OP_POP", offset);
        case OP_PRINT:
            return simple_instruction("OP_PRINT", offset);

        case OP_GET_LOCAL:
            return short_instruction("OP_GET_LOCAL", chunk, offset);
        case OP_SET_LOCAL:
            return short_instruction("OP_SET_LOCAL", chunk, offset);
        case OP_GET_GLOBAL:
            return constant_instruction("OP_GET_GLOBAL", chunk, offset);
        case OP_SET_GLOBAL:
            return constant_instruction("OP_SET_GLOBAL", chunk, offset);
        case OP_DEFINE_GLOBAL:
            return constant_instruction("OP_DEFINE_GLOBAL", chunk, offset);

        case OP_GET_UPVALUE:
            return byte_instruction("OP_GET_UPVALUE", chunk, offset);
        case OP_SET_UPVALUE:
            return byte_instruction("OP_SET_UPVALUE", chunk, offset);
        case OP_CLOSE_UPVALUE:
            return simple_instruction("OP_CLOSE_UPVALUE", offset);

        case OP_JUMP:
            return jump_instruction("OP_JUMP", 1, chunk, offset);
        case OP_JUMP_IF_FALSE:
            return jump_instruction("OP_JUMP_IF_FALSE", 1, chunk, offset);
        case OP_LOOP:
            return jump_instruction("OP_LOOP", -1, chunk, offset);

        case OP_CALL:
            return byte_instruction("OP_CALL", chunk, offset);
        case OP_RETURN:
            return simple_instruction("OP_RETURN", offset);

        case OP_CLOSURE: {
            offset++;
            uint16_t constant = (uint16_t)(chunk->code[offset] | (chunk->code[offset + 1] << 8));
            offset += 2;
            printf("%-20s %5d ", "OP_CLOSURE", constant);
            if (constant < (uint16_t)chunk->const_count) {
                print_value(chunk->constants[constant]);
            }
            printf("\n");

            // Print upvalue data
            if (constant < (uint16_t)chunk->const_count) {
                Value fn_val = chunk->constants[constant];
                if (IS_PTR(fn_val)) {
                    ObjFunction* fn = (ObjFunction*)AS_PTR(fn_val);
                    for (int j = 0; j < fn->upvalue_count; j++) {
                        int is_local = chunk->code[offset++];
                        int index = chunk->code[offset++];
                        printf("%04d    |                     %s %d\n",
                               offset - 2, is_local ? "local" : "upvalue", index);
                    }
                }
            }
            return offset;
        }

        case OP_NEW_LIST:
            return short_instruction("OP_NEW_LIST", chunk, offset);
        case OP_NEW_MAP:
            return short_instruction("OP_NEW_MAP", chunk, offset);

        case OP_GET_INDEX:
            return simple_instruction("OP_GET_INDEX", offset);
        case OP_SET_INDEX:
            return simple_instruction("OP_SET_INDEX", offset);
        case OP_GET_FIELD:
            return constant_instruction("OP_GET_FIELD", chunk, offset);
        case OP_SET_FIELD:
            return constant_instruction("OP_SET_FIELD", chunk, offset);

        case OP_ITER_INIT:
            return simple_instruction("OP_ITER_INIT", offset);
        case OP_ITER_NEXT:
            return jump_instruction("OP_ITER_NEXT", 1, chunk, offset);

        case OP_POWER:
            return simple_instruction("OP_POWER", offset);

        default:
            printf("Unknown opcode %d\n", instruction);
            return offset + 1;
    }
}
