#include "vebc_writer.h"
#include "sha256.h"
#include "value.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_u16_le(uint8_t* buf, uint16_t val) {
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
}

static void write_u32_le(uint8_t* buf, uint32_t val) {
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
    buf[2] = (uint8_t)((val >> 16) & 0xFF);
    buf[3] = (uint8_t)((val >> 24) & 0xFF);
}

static void write_i64_le(uint8_t* buf, int64_t val) {
    uint64_t u = (uint64_t)val;
    for (int i = 0; i < 8; i++) buf[i] = (uint8_t)(u >> (i * 8));
}

static void write_f64_le(uint8_t* buf, double val) { memcpy(buf, &val, 8); }

static void builder_ensure_instr_capacity(VebcBuilder* b, uint32_t needed) {
    while (b->instr_count + needed > b->instr_capacity) {
        b->instr_capacity = b->instr_capacity < 1024 ? 1024 : b->instr_capacity * 2;
        b->instructions = (uint8_t*)realloc(b->instructions, b->instr_capacity);
    }
}

static void builder_ensure_line_capacity(VebcBuilder* b, uint32_t needed) {
    while (b->line_entry_count + needed > b->line_entry_capacity) {
        b->line_entry_capacity = b->line_entry_capacity < 256 ? 256 : b->line_entry_capacity * 2;
        b->line_entries = (VebcLineEntry*)realloc(b->line_entries, b->line_entry_capacity * sizeof(VebcLineEntry));
    }
}

void vebc_builder_init(VebcBuilder* b) {
    memset(b, 0, sizeof(VebcBuilder));
    b->instr_capacity = 4096;
    b->instructions = (uint8_t*)malloc(b->instr_capacity);
    b->line_entry_capacity = 256;
    b->line_entries = (VebcLineEntry*)malloc(b->line_entry_capacity * sizeof(VebcLineEntry));
}

uint32_t vebc_builder_add_string(VebcBuilder* b, const char* str, uint32_t len) {
    for (uint32_t i = 0; i < b->string_count; i++) {
        if (b->strings[i].length == len && memcmp(b->strings[i].data, str, len) == 0) return i;
    }
    if (b->string_count >= VEBC_MAX_STRINGS) { fprintf(stderr, "Error: too many strings\n"); return 0; }
    uint32_t idx = b->string_count;
    b->strings[idx].data = (char*)malloc(len);
    memcpy(b->strings[idx].data, str, len);
    b->strings[idx].length = len;
    b->string_count++;
    return idx;
}

void vebc_builder_add_function(VebcBuilder* b, ObjFunction* fn) {
    if (b->func_count >= VEBC_MAX_FUNCTIONS) { fprintf(stderr, "Error: too many functions\n"); return; }
    VebcFunction* vf = &b->functions[b->func_count];
    if (fn->name != NULL)
        vf->name_idx = vebc_builder_add_string(b, fn->name->data, fn->name->length);
    else
        vf->name_idx = vebc_builder_add_string(b, "<script>", 8);
    vf->num_params = fn->arity;
    vf->num_upvalues = fn->upvalue_count;
    vf->num_regs = (uint16_t)(fn->arity + 16);
    vf->code_offset = b->instr_count;
    vf->code_length = (uint32_t)fn->chunk.count;
    builder_ensure_instr_capacity(b, (uint32_t)fn->chunk.count);
    if (fn->chunk.count > 0) {
        memcpy(b->instructions + b->instr_count, fn->chunk.code, fn->chunk.count);
        b->instr_count += (uint32_t)fn->chunk.count;
    }
    vf->line_table_offset = b->line_entry_count;
    uint32_t lstart = b->line_entry_count;
    if (fn->chunk.count > 0 && fn->chunk.lines != NULL) {
        int pl = -1;
        for (int i = 0; i < fn->chunk.count; i++) {
            int ln = fn->chunk.lines[i];
            if (ln != pl) {
                builder_ensure_line_capacity(b, 1);
                VebcLineEntry* e = &b->line_entries[b->line_entry_count];
                e->code_offset = (uint32_t)i;
                e->source_line = (uint32_t)ln;
                b->line_entry_count++;
                pl = ln;
            }
        }
    }
    vf->line_table_length = b->line_entry_count - lstart;
    for (int i = 0; i < fn->chunk.const_count; i++) {
        if (b->const_count >= VEBC_MAX_CONSTANTS) break;
        Value val = fn->chunk.constants[i];
        VebcConstant* vc = &b->constants[b->const_count];
        if (IS_INT(val)) { vc->tag = CONST_TAG_INT; vc->as.int_val = AS_INT(val); b->const_count++; }
        else if (IS_FLOAT(val)) { vc->tag = CONST_TAG_FLOAT; vc->as.float_val = AS_DOUBLE(val); b->const_count++; }
        else if (IS_STRING(val)) { ObjString* s = AS_STRING(val); vc->tag = CONST_TAG_STRING; vc->as.string_idx = vebc_builder_add_string(b, s->data, s->length); b->const_count++; }
        else if (IS_FUNCTION(val)) { vc->tag = CONST_TAG_FUNC_REF; vc->as.func_idx = b->func_count; b->const_count++; }
        else if (IS_BYTES(val)) { ObjBytes* by = AS_BYTES(val); vc->tag = CONST_TAG_BYTES; vc->as.bytes_val.length = by->length; vc->as.bytes_val.data = (uint8_t*)malloc(by->length); memcpy(vc->as.bytes_val.data, by->data, by->length); b->const_count++; }
    }
    b->func_count++;
}

void vebc_builder_add_asset(VebcBuilder* b, const char* path, uint8_t* data, uint32_t len) {
    if (b->asset_count >= VEBC_MAX_ASSETS) { fprintf(stderr, "Error: too many assets\n"); return; }
    VebcAsset* a = &b->assets[b->asset_count];
    a->path_idx = vebc_builder_add_string(b, path, (uint32_t)strlen(path));
    a->length = len;
    a->data = (uint8_t*)malloc(len);
    memcpy(a->data, data, len);
    b->asset_count++;
}

bool vebc_builder_write(VebcBuilder* b, const char* output_path) {
    FILE* f = fopen(output_path, "wb");
    if (!f) { fprintf(stderr, "Error: could not open '%s' for writing\n", output_path); return false; }
    size_t size = 64 + 4 + 4 + 4 + (size_t)b->func_count * 24 + 4 + (size_t)b->upvalue_count * 6 + 4 + b->instr_count + 4 + (size_t)b->line_entry_count * 8 + 4;
    for (uint32_t i = 0; i < b->const_count; i++) {
        size += 1;
        switch (b->constants[i].tag) {
            case CONST_TAG_INT: case CONST_TAG_FLOAT: size += 8; break;
            case CONST_TAG_STRING: case CONST_TAG_FUNC_REF: size += 4; break;
            case CONST_TAG_BYTES: size += 4 + b->constants[i].as.bytes_val.length; break;
            default: break;
        }
    }
    for (uint32_t i = 0; i < b->string_count; i++) size += 4 + b->strings[i].length;
    for (uint32_t i = 0; i < b->asset_count; i++) size += 4 + 4 + b->assets[i].length;
    uint8_t* buf = (uint8_t*)calloc(size, 1);
    if (!buf) { fclose(f); return false; }
    size_t pos = 0;
    buf[0] = 0x56; buf[1] = 0x45; buf[2] = 0x42; buf[3] = 0x43;
    write_u16_le(buf + 4, 1); write_u16_le(buf + 6, 0);
    pos = 64;
    write_u32_le(buf + pos, b->const_count); pos += 4;
    for (uint32_t i = 0; i < b->const_count; i++) {
        VebcConstant* c = &b->constants[i]; buf[pos++] = c->tag;
        switch (c->tag) {
            case CONST_TAG_INT: write_i64_le(buf + pos, c->as.int_val); pos += 8; break;
            case CONST_TAG_FLOAT: write_f64_le(buf + pos, c->as.float_val); pos += 8; break;
            case CONST_TAG_STRING: write_u32_le(buf + pos, c->as.string_idx); pos += 4; break;
            case CONST_TAG_FUNC_REF: write_u32_le(buf + pos, c->as.func_idx); pos += 4; break;
            case CONST_TAG_BYTES: write_u32_le(buf + pos, c->as.bytes_val.length); pos += 4; memcpy(buf + pos, c->as.bytes_val.data, c->as.bytes_val.length); pos += c->as.bytes_val.length; break;
            default: break;
        }
    }
    write_u32_le(buf + pos, b->string_count); pos += 4;
    for (uint32_t i = 0; i < b->string_count; i++) {
        write_u32_le(buf + pos, b->strings[i].length); pos += 4;
        memcpy(buf + pos, b->strings[i].data, b->strings[i].length); pos += b->strings[i].length;
    }
    write_u32_le(buf + pos, b->func_count); pos += 4;
    for (uint32_t i = 0; i < b->func_count; i++) {
        VebcFunction* vf = &b->functions[i];
        write_u32_le(buf + pos, vf->name_idx); pos += 4;
        write_u16_le(buf + pos, vf->num_regs); pos += 2;
        buf[pos++] = vf->num_params; buf[pos++] = vf->num_upvalues;
        write_u32_le(buf + pos, vf->code_offset); pos += 4;
        write_u32_le(buf + pos, vf->code_length); pos += 4;
        write_u32_le(buf + pos, vf->line_table_offset); pos += 4;
        write_u32_le(buf + pos, vf->line_table_length); pos += 4;
    }
    write_u32_le(buf + pos, b->upvalue_count); pos += 4;
    for (uint32_t i = 0; i < b->upvalue_count; i++) {
        VebcUpvalue* uv = &b->upvalues[i];
        write_u32_le(buf + pos, uv->src_idx); pos += 4;
        buf[pos++] = uv->slot; buf[pos++] = uv->is_local;
    }
    write_u32_le(buf + pos, b->instr_count); pos += 4;
    if (b->instr_count > 0) { memcpy(buf + pos, b->instructions, b->instr_count); pos += b->instr_count; }
    write_u32_le(buf + pos, b->line_entry_count); pos += 4;
    for (uint32_t i = 0; i < b->line_entry_count; i++) {
        write_u32_le(buf + pos, b->line_entries[i].code_offset); pos += 4;
        write_u32_le(buf + pos, b->line_entries[i].source_line); pos += 4;
    }
    write_u32_le(buf + pos, b->asset_count); pos += 4;
    for (uint32_t i = 0; i < b->asset_count; i++) {
        VebcAsset* as = &b->assets[i];
        write_u32_le(buf + pos, as->path_idx); pos += 4;
        write_u32_le(buf + pos, as->length); pos += 4;
        if (as->length > 0) { memcpy(buf + pos, as->data, as->length); pos += as->length; }
    }
    uint8_t hash[32];
    sha256_compute(buf + 64, pos - 64, hash);
    memcpy(buf + 8, hash, 32);
    size_t written = fwrite(buf, 1, pos, f);
    free(buf); fclose(f);
    return written == pos;
}

void vebc_builder_destroy(VebcBuilder* b) {
    for (uint32_t i = 0; i < b->string_count; i++) free(b->strings[i].data);
    for (uint32_t i = 0; i < b->const_count; i++) {
        if (b->constants[i].tag == CONST_TAG_BYTES) free(b->constants[i].as.bytes_val.data);
    }
    for (uint32_t i = 0; i < b->asset_count; i++) free(b->assets[i].data);
    free(b->instructions);
    free(b->line_entries);
    memset(b, 0, sizeof(VebcBuilder));
}
