#ifndef VEK_COMPILER_H
#define VEK_COMPILER_H

#include "common.h"
#include "chunk.h"
#include "lexer.h"
#include "object.h"

// Compile source code into an ObjFunction (the top-level script function).
// Returns NULL if compilation fails.
ObjFunction* compile(const char* source);

#endif // VEK_COMPILER_H
