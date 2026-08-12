#ifndef VEK_COMMON_H
#define VEK_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>

// Version
#define VEK_VERSION_MAJOR 0
#define VEK_VERSION_MINOR 1
#define VEK_VERSION_PATCH 0
#define VEK_VERSION_STRING "0.1.0"

// Utility macros for dynamic arrays
#define GROW_CAPACITY(capacity) \
    ((capacity) < 8 ? 8 : (capacity) * 2)

#define GROW_ARRAY(type, pointer, old_count, new_count) \
    (type*)vek_realloc(pointer, sizeof(type) * (old_count), sizeof(type) * (new_count))

#define FREE_ARRAY(type, pointer, old_count) \
    vek_realloc(pointer, sizeof(type) * (old_count), 0)

// Forward declarations
typedef uint64_t Value;
typedef struct ObjHeader ObjHeader;
typedef struct ObjString ObjString;
typedef struct ObjList ObjList;
typedef struct ObjMap ObjMap;
typedef struct ObjBytes ObjBytes;
typedef struct ObjClosure ObjClosure;
typedef struct ObjFunction ObjFunction;
typedef struct Page Page;
typedef struct GC GC;

// Memory functions (declared here, defined in memory.c)
void* vek_alloc(size_t size);
void* vek_realloc(void* pointer, size_t old_size, size_t new_size);
void  vek_free(void* pointer, size_t size);

#endif // VEK_COMMON_H
