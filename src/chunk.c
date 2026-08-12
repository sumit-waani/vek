#include "chunk.h"
#include "memory.h"

void chunk_init(Chunk* chunk) {
    chunk->code = NULL;
    chunk->count = 0;
    chunk->capacity = 0;
    chunk->constants = NULL;
    chunk->const_count = 0;
    chunk->const_capacity = 0;
    chunk->lines = NULL;
    chunk->line_count = 0;
    chunk->line_capacity = 0;
}

void chunk_write(Chunk* chunk, uint8_t byte, int line) {
    if (chunk->count >= chunk->capacity) {
        int old_cap = chunk->capacity;
        chunk->capacity = GROW_CAPACITY(old_cap);
        chunk->code = GROW_ARRAY(uint8_t, chunk->code, old_cap, chunk->capacity);
    }
    if (chunk->line_count >= chunk->line_capacity) {
        int old_cap = chunk->line_capacity;
        chunk->line_capacity = GROW_CAPACITY(old_cap);
        chunk->lines = GROW_ARRAY(int, chunk->lines, old_cap, chunk->line_capacity);
    }
    chunk->code[chunk->count] = byte;
    chunk->lines[chunk->line_count] = line;
    chunk->count++;
    chunk->line_count++;
}

int chunk_add_constant(Chunk* chunk, Value value) {
    if (chunk->const_count >= chunk->const_capacity) {
        int old_cap = chunk->const_capacity;
        chunk->const_capacity = GROW_CAPACITY(old_cap);
        chunk->constants = GROW_ARRAY(Value, chunk->constants, old_cap, chunk->const_capacity);
    }
    chunk->constants[chunk->const_count] = value;
    return chunk->const_count++;
}

void chunk_free(Chunk* chunk) {
    if (chunk->code) {
        FREE_ARRAY(uint8_t, chunk->code, chunk->capacity);
    }
    if (chunk->constants) {
        FREE_ARRAY(Value, chunk->constants, chunk->const_capacity);
    }
    if (chunk->lines) {
        FREE_ARRAY(int, chunk->lines, chunk->line_capacity);
    }
    chunk_init(chunk);
}
