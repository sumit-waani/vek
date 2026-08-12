#ifndef VEK_DEBUG_H
#define VEK_DEBUG_H

#include "chunk.h"

// Disassemble an entire chunk (prints all instructions)
void disassemble_chunk(Chunk* chunk, const char* name);

// Disassemble a single instruction at the given offset.
// Returns the offset of the next instruction.
int disassemble_instruction(Chunk* chunk, int offset);

#endif // VEK_DEBUG_H
