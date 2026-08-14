#include "vebc_loader.h"
#include "vebc_writer.h"
#include "sha256.h"
#include "value.h"
#include "chunk.h"
#include "gc.h"
#include "memory.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

// Little-endian readers
static uint16_t read_u16_le(const uint8_t* buf) {
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static uint32_t read_u32_le(const uint8_t* buf) {
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

static int64_t read_i64_le(const uint8_t* buf) {
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) {
        u |= (uint64_t)buf[i] << (i * 8);
    }
    return (int64_t)u;
}

static double read_f64_le(const uint8_t* buf) {
    double d;
    memcpy(&d, buf, 8);
    return d;
}

VebcFile* vebc_load(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Error: could not open '%s': %s\n", path, strerror(errno));
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        fprintf(stderr, "Error: could not stat '%s': %s\n", path, strerror(errno));
        close(fd);
        return NULL;
    }

    size_t file_size = (size_t)st.st_size;
    if (file_size < 64) {
        fprintf(stderr, "Error: '%s' is too small to be a valid .vebc file\n", path);
        close(fd);
        return NULL;
    }

    uint8_t* data = (uint8_t*)mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (data == MAP_FAILED) {
        fprintf(stderr, "Error: could not mmap '%s': %s\n", path, strerror(errno));
        return NULL;
    }

    // Validate magic bytes: "VEBC" = 0x56, 0x45, 0x42, 0x43
    if (data[0] != 0x56 || data[1] != 0x45 || data[2] != 0x42 || data[3] != 0x43) {
        fprintf(stderr, "Error: '%s' is not a valid .vebc file (bad magic)\n", path);
        munmap(data, file_size);
        return NULL;
    }

    VebcFile* file = (VebcFile*)calloc(1, sizeof(VebcFile));
    if (!file) {
        munmap(data, file_size);
        return NULL;
    }

    file->data = data;
    file->data_size = file_size;
    file->version = read_u16_le(data + 4);
    file->flags = read_u16_le(data + 6);
    memcpy(file->sha256, data + 8, 32);

    // Validate version
    if (file->version != 1) {
        fprintf(stderr, "Error: unsupported .vebc version %u (expected 1)\n", file->version);
        munmap(data, file_size);
        free(file);
        return NULL;
    }

    // Parse sections starting at offset 64
    size_t pos = 64;

    // Constants table
    if (pos + 4 > file_size) goto truncated;
    file->const_count = read_u32_le(data + pos);
    pos += 4;
    file->const_offset = pos;

    // Skip over constants to find next section
    for (uint32_t i = 0; i < file->const_count; i++) {
        if (pos + 1 > file_size) goto truncated;
        uint8_t tag = data[pos++];
        switch (tag) {
            case CONST_TAG_INT:
            case CONST_TAG_FLOAT:
                if (pos + 8 > file_size) goto truncated;
                pos += 8;
                break;
            case CONST_TAG_STRING:
            case CONST_TAG_FUNC_REF:
                if (pos + 4 > file_size) goto truncated;
                pos += 4;
                break;
            case CONST_TAG_BYTES: {
                if (pos + 4 > file_size) goto truncated;
                uint32_t len = read_u32_le(data + pos);
                pos += 4;
                if (pos + len > file_size) goto truncated;
                pos += len;
                break;
            }
            default:
                fprintf(stderr, "Error: unknown constant tag 0x%02x\n", tag);
                goto error;
        }
    }

    // Strings table
    if (pos + 4 > file_size) goto truncated;
    file->string_count = read_u32_le(data + pos);
    pos += 4;
    file->string_offset = pos;

    // Parse strings into array for easy access
    file->strings = (char**)calloc(file->string_count, sizeof(char*));
    file->string_lens = (uint32_t*)calloc(file->string_count, sizeof(uint32_t));
    if (!file->strings || !file->string_lens) goto error;

    for (uint32_t i = 0; i < file->string_count; i++) {
        if (pos + 4 > file_size) goto truncated;
        uint32_t slen = read_u32_le(data + pos);
        pos += 4;
        if (pos + slen > file_size) goto truncated;
        file->strings[i] = (char*)(data + pos);
        file->string_lens[i] = slen;
        pos += slen;
    }

    // Function table
    if (pos + 4 > file_size) goto truncated;
    file->func_count = read_u32_le(data + pos);
    pos += 4;
    file->func_offset = pos;

    // Each function entry is 24 bytes: name_idx(4) + num_regs(2) + num_params(1) + num_upvalues(1)
    //   + code_offset(4) + code_length(4) + line_table_offset(4) + line_table_length(4)
    if (pos + (size_t)file->func_count * 24 > file_size) goto truncated;
    pos += (size_t)file->func_count * 24;

    // Upvalue table
    if (pos + 4 > file_size) goto truncated;
    file->upvalue_count = read_u32_le(data + pos);
    pos += 4;
    file->upvalue_offset = pos;

    // Each upvalue entry is 6 bytes: src_idx(4) + slot(1) + is_local(1)
    if (pos + (size_t)file->upvalue_count * 6 > file_size) goto truncated;
    pos += (size_t)file->upvalue_count * 6;

    // Instruction section
    if (pos + 4 > file_size) goto truncated;
    file->instr_length = read_u32_le(data + pos);
    pos += 4;
    file->instr_offset = pos;

    if (pos + file->instr_length > file_size) goto truncated;
    pos += file->instr_length;

    // Line table
    if (pos + 4 > file_size) goto truncated;
    file->line_count = read_u32_le(data + pos);
    pos += 4;
    file->line_offset = pos;

    // Each line entry is 8 bytes: code_offset(4) + source_line(4)
    if (pos + (size_t)file->line_count * 8 > file_size) goto truncated;
    pos += (size_t)file->line_count * 8;

    // Asset section
    if (pos + 4 > file_size) goto truncated;
    file->asset_count = read_u32_le(data + pos);
    pos += 4;
    file->asset_offset = pos;

    return file;

truncated:
    fprintf(stderr, "Error: '%s' is truncated or corrupted\n", path);
error:
    if (file->strings) free(file->strings);
    if (file->string_lens) free(file->string_lens);
    munmap(data, file_size);
    free(file);
    return NULL;
}

bool vebc_verify(VebcFile* file) {
    if (!file || file->data_size < 64) return false;

    uint8_t computed[32];
    sha256_compute(file->data + 64, file->data_size - 64, computed);

    return memcmp(computed, file->sha256, 32) == 0;
}

ObjFunction* vebc_to_function(VebcFile* file) {
    if (!file || file->func_count == 0) {
        fprintf(stderr, "Error: .vebc file has no functions\n");
        return NULL;
    }

    // Read function 0 (entry point) from the function table
    const uint8_t* func_entry = file->data + file->func_offset;
    uint32_t name_idx = read_u32_le(func_entry + 0);
    // uint16_t num_regs = read_u16_le(func_entry + 4); // not used directly
    uint8_t num_params = func_entry[6];
    uint8_t num_upvalues = func_entry[7];
    uint32_t code_offset = read_u32_le(func_entry + 8);
    uint32_t code_length = read_u32_le(func_entry + 12);
    uint32_t line_table_offset = read_u32_le(func_entry + 16);
    uint32_t line_table_length = read_u32_le(func_entry + 20);

    // Create the ObjFunction
    ObjFunction* fn = obj_function_new();
    gc_push_root(OBJ_VAL(fn));

    fn->arity = num_params;
    fn->upvalue_count = num_upvalues;

    // Set function name
    if (name_idx < file->string_count) {
        fn->name = obj_string_new(file->strings[name_idx], file->string_lens[name_idx]);
    }

    // Reconstruct the chunk - copy bytecode
    if (code_length > 0) {
        // Verify code_offset is within instruction section bounds
        if (code_offset + code_length > file->instr_length) {
            fprintf(stderr, "Error: function code_offset + code_length exceeds instruction section\n");
            gc_pop_root();
            return NULL;
        }

        const uint8_t* code_src = file->data + file->instr_offset + code_offset;
        for (uint32_t i = 0; i < code_length; i++) {
            chunk_write(&fn->chunk, code_src[i], 0); // line filled in below
        }
    }

    // Reconstruct line information from line table entries
    if (line_table_length > 0 && line_table_offset + line_table_length <= file->line_count) {
        const uint8_t* line_src = file->data + file->line_offset + (size_t)line_table_offset * 8;

        // Build a map from code offsets to source lines
        // Each entry: code_offset(4) + source_line(4)
        // Fill in lines array for the chunk
        uint32_t entry_idx = 0;
        uint32_t current_line = 1;

        for (uint32_t i = 0; i < code_length; i++) {
            // Advance to the next line entry if we've reached its offset
            while (entry_idx < line_table_length) {
                uint32_t entry_code_offset = read_u32_le(line_src + entry_idx * 8);
                uint32_t entry_source_line = read_u32_le(line_src + entry_idx * 8 + 4);
                if (entry_code_offset <= i) {
                    current_line = entry_source_line;
                    entry_idx++;
                } else {
                    break;
                }
            }
            fn->chunk.lines[i] = (int)current_line;
        }
    }

    // Reconstruct constants from the constants table
    size_t pos = file->const_offset;
    for (uint32_t i = 0; i < file->const_count; i++) {
        uint8_t tag = file->data[pos++];
        Value val = VAL_NIL;

        switch (tag) {
            case CONST_TAG_INT:
                val = INT_VAL(read_i64_le(file->data + pos));
                pos += 8;
                break;
            case CONST_TAG_FLOAT:
                val = FLOAT_VAL(read_f64_le(file->data + pos));
                pos += 8;
                break;
            case CONST_TAG_STRING: {
                uint32_t str_idx = read_u32_le(file->data + pos);
                pos += 4;
                if (str_idx < file->string_count) {
                    ObjString* s = obj_string_new(file->strings[str_idx],
                                                  file->string_lens[str_idx]);
                    val = OBJ_VAL(s);
                }
                break;
            }
            case CONST_TAG_FUNC_REF: {
                uint32_t func_idx = read_u32_le(file->data + pos);
                pos += 4;
                fprintf(stderr, "warning: unresolved function reference (index %u) in .vebc at constant %u - loaded as nil\n", func_idx, i);
                break;
            }
            case CONST_TAG_BYTES: {
                uint32_t blen = read_u32_le(file->data + pos);
                pos += 4;
                ObjBytes* b = obj_bytes_new(file->data + pos, blen);
                val = OBJ_VAL(b);
                pos += blen;
                break;
            }
            default:
                break;
        }

        chunk_add_constant(&fn->chunk, val);
    }

    gc_pop_root();
    return fn;
}

void vebc_free(VebcFile* file) {
    if (!file) return;

    if (file->data && file->data_size > 0) {
        munmap(file->data, file->data_size);
    }

    free(file->strings);
    free(file->string_lens);
    free(file);
}
