#ifndef VEK_VEBC_WRITER_H
#define VEK_VEBC_WRITER_H

#include "common.h"
#include "object.h"

#define VEBC_MAX_STRINGS    4096
#define VEBC_MAX_CONSTANTS  4096
#define VEBC_MAX_FUNCTIONS  1024
#define VEBC_MAX_UPVALUES   4096
#define VEBC_MAX_ASSETS     1024

#define CONST_TAG_INT       0x01
#define CONST_TAG_FLOAT     0x02
#define CONST_TAG_STRING    0x03
#define CONST_TAG_BYTES     0x04
#define CONST_TAG_FUNC_REF  0x05

typedef struct {
    uint8_t tag;
    union {
        int64_t int_val;
        double  float_val;
        uint32_t string_idx;
        uint32_t func_idx;
        struct {
            uint8_t* data;
            uint32_t length;
        } bytes_val;
    } as;
} VebcConstant;

typedef struct {
    uint32_t name_idx;
    uint16_t num_regs;
    uint8_t  num_params;
    uint8_t  num_upvalues;
    uint32_t code_offset;
    uint32_t code_length;
    uint32_t line_table_offset;
    uint32_t line_table_length;
} VebcFunction;

typedef struct {
    uint32_t src_idx;
    uint8_t  slot;
    uint8_t  is_local;
} VebcUpvalue;

typedef struct {
    uint32_t code_offset;
    uint32_t source_line;
} VebcLineEntry;

typedef struct {
    uint32_t path_idx;
    uint32_t length;
    uint8_t* data;
} VebcAsset;

typedef struct {
    char*    data;
    uint32_t length;
} VebcString;

typedef struct {
    VebcString   strings[VEBC_MAX_STRINGS];
    uint32_t     string_count;
    VebcConstant constants[VEBC_MAX_CONSTANTS];
    uint32_t     const_count;
    VebcFunction functions[VEBC_MAX_FUNCTIONS];
    uint32_t     func_count;
    VebcUpvalue  upvalues[VEBC_MAX_UPVALUES];
    uint32_t     upvalue_count;
    uint8_t*     instructions;
    uint32_t     instr_count;
    uint32_t     instr_capacity;
    VebcLineEntry* line_entries;
    uint32_t       line_entry_count;
    uint32_t       line_entry_capacity;
    VebcAsset    assets[VEBC_MAX_ASSETS];
    uint32_t     asset_count;
} VebcBuilder;

void vebc_builder_init(VebcBuilder* b);
uint32_t vebc_builder_add_string(VebcBuilder* b, const char* str, uint32_t len);
void vebc_builder_add_function(VebcBuilder* b, ObjFunction* fn);
void vebc_builder_add_asset(VebcBuilder* b, const char* path, uint8_t* data, uint32_t len);
bool vebc_builder_write(VebcBuilder* b, const char* output_path);
void vebc_builder_destroy(VebcBuilder* b);

#endif // VEK_VEBC_WRITER_H
