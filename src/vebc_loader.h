#ifndef VEK_VEBC_LOADER_H
#define VEK_VEBC_LOADER_H

#include "common.h"
#include "object.h"

// Parsed .vebc file structure
typedef struct {
    uint8_t* data;          // mmap'd file data
    size_t   data_size;     // total file size

    // Header fields
    uint16_t version;
    uint16_t flags;
    uint8_t  sha256[32];

    // Section counts (parsed from file)
    uint32_t const_count;
    uint32_t string_count;
    uint32_t func_count;
    uint32_t upvalue_count;
    uint32_t instr_length;
    uint32_t line_count;
    uint32_t asset_count;

    // Parsed section offsets into mmap'd data
    size_t   const_offset;
    size_t   string_offset;
    size_t   func_offset;
    size_t   upvalue_offset;
    size_t   instr_offset;
    size_t   line_offset;
    size_t   asset_offset;

    // Reconstructed string table
    char**    strings;
    uint32_t* string_lens;
} VebcFile;

// Load a .vebc file from disk (mmap + parse header)
VebcFile* vebc_load(const char* path);

// Verify integrity: compute SHA-256 of bytes 64..EOF and compare with header hash
bool vebc_verify(VebcFile* file);

// Reconstruct an ObjFunction from the loaded bytecode (entry point = function 0)
ObjFunction* vebc_to_function(VebcFile* file);

// Free the loaded file (munmap + free allocations)
void vebc_free(VebcFile* file);

#endif // VEK_VEBC_LOADER_H
