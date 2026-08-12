#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

// ---- CRC32 for gzip trailer ----

static uint32_t crc32_table[256];
static bool crc32_initialized = false;

static void crc32_init_table(void) {
    if (crc32_initialized) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            if (c & 1) c = 0xEDB88320 ^ (c >> 1);
            else c >>= 1;
        }
        crc32_table[i] = c;
    }
    crc32_initialized = true;
}

static uint32_t crc32_compute(const uint8_t* data, size_t len) {
    crc32_init_table();
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

// ---- DEFLATE stored blocks (type 0 - no compression, just framing) ----
// This is a valid DEFLATE stream using stored blocks.
// Max block size is 65535 bytes.

static uint8_t* deflate_stored(const uint8_t* data, size_t data_len, size_t* out_len) {
    // Calculate number of blocks needed
    size_t num_blocks = (data_len == 0) ? 1 : (data_len + 65534) / 65535;
    // Each block: 1 byte header + 2 bytes LEN + 2 bytes NLEN + data
    size_t total = num_blocks * 5 + data_len;

    uint8_t* out = (uint8_t*)malloc(total);
    if (!out) { *out_len = 0; return NULL; }

    size_t pos = 0;
    size_t remaining = data_len;
    size_t src_pos = 0;

    while (remaining > 0 || src_pos == 0) {
        size_t block_size = remaining > 65535 ? 65535 : remaining;
        bool is_last = (remaining <= 65535);

        // Block header byte: BFINAL (bit 0) | BTYPE=00 (bits 1-2)
        out[pos++] = is_last ? 0x01 : 0x00;

        // LEN (2 bytes, little-endian)
        out[pos++] = (uint8_t)(block_size & 0xFF);
        out[pos++] = (uint8_t)((block_size >> 8) & 0xFF);

        // NLEN (one's complement of LEN)
        uint16_t nlen = (uint16_t)(~block_size);
        out[pos++] = (uint8_t)(nlen & 0xFF);
        out[pos++] = (uint8_t)((nlen >> 8) & 0xFF);

        // Data
        if (block_size > 0) {
            memcpy(out + pos, data + src_pos, block_size);
            pos += block_size;
        }

        src_pos += block_size;
        remaining -= block_size;

        if (data_len == 0) break; // handle empty input
    }

    *out_len = pos;
    return out;
}

// ---- INFLATE for stored blocks ----

static uint8_t* inflate_stored(const uint8_t* data, size_t data_len, size_t* out_len) {
    // First pass: calculate total output size
    size_t total_size = 0;
    size_t pos = 0;

    while (pos < data_len) {
        if (pos >= data_len) break;
        uint8_t header = data[pos++];
        (void)header; // BFINAL flag

        if (pos + 4 > data_len) break;
        uint16_t len = (uint16_t)(data[pos] | (data[pos + 1] << 8));
        pos += 4; // skip LEN and NLEN

        total_size += len;
        pos += len;

        if (header & 0x01) break; // BFINAL
    }

    uint8_t* out = (uint8_t*)malloc(total_size + 1);
    if (!out) { *out_len = 0; return NULL; }

    // Second pass: extract data
    pos = 0;
    size_t out_pos = 0;

    while (pos < data_len) {
        uint8_t header = data[pos++];

        if (pos + 4 > data_len) break;
        uint16_t len = (uint16_t)(data[pos] | (data[pos + 1] << 8));
        pos += 4; // skip LEN and NLEN

        if (pos + len > data_len) break;
        if (len > 0) {
            memcpy(out + out_pos, data + pos, len);
            out_pos += len;
        }
        pos += len;

        if (header & 0x01) break; // BFINAL
    }

    *out_len = out_pos;
    return out;
}

// ---- compress.deflate(string) ----

static Value native_compress_deflate(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ObjString* input = AS_STRING(args[0]);

    size_t out_len;
    uint8_t* compressed = deflate_stored((const uint8_t*)input->data,
                                          input->length, &out_len);
    if (!compressed) return VAL_NIL;

    ObjString* result = obj_string_new((const char*)compressed, (uint32_t)out_len);
    free(compressed);
    return OBJ_VAL(result);
}

// ---- compress.inflate(string) ----

static Value native_compress_inflate(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ObjString* input = AS_STRING(args[0]);

    size_t out_len;
    uint8_t* decompressed = inflate_stored((const uint8_t*)input->data,
                                            input->length, &out_len);
    if (!decompressed) return VAL_NIL;

    ObjString* result = obj_string_new((const char*)decompressed, (uint32_t)out_len);
    free(decompressed);
    return OBJ_VAL(result);
}

// ---- compress.gzip(string) ----
// Gzip format: 10-byte header + deflate data + 8-byte trailer (CRC32 + ISIZE)

static Value native_compress_gzip(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ObjString* input = AS_STRING(args[0]);

    size_t deflate_len;
    uint8_t* deflated = deflate_stored((const uint8_t*)input->data,
                                        input->length, &deflate_len);
    if (!deflated) return VAL_NIL;

    // Total size: 10 (header) + deflate_len + 8 (trailer)
    size_t total = 10 + deflate_len + 8;
    uint8_t* out = (uint8_t*)malloc(total);
    if (!out) { free(deflated); return VAL_NIL; }

    // Gzip header (minimal, 10 bytes)
    out[0] = 0x1F; // magic
    out[1] = 0x8B; // magic
    out[2] = 0x08; // compression method (deflate)
    out[3] = 0x00; // flags
    out[4] = 0x00; // mtime
    out[5] = 0x00;
    out[6] = 0x00;
    out[7] = 0x00;
    out[8] = 0x00; // extra flags
    out[9] = 0xFF; // OS (unknown)

    // Deflate data
    memcpy(out + 10, deflated, deflate_len);
    free(deflated);

    // Trailer: CRC32 + ISIZE (both little-endian)
    uint32_t crc = crc32_compute((const uint8_t*)input->data, input->length);
    uint32_t isize = (uint32_t)(input->length & 0xFFFFFFFF);

    size_t trailer_pos = 10 + deflate_len;
    out[trailer_pos + 0] = (uint8_t)(crc & 0xFF);
    out[trailer_pos + 1] = (uint8_t)((crc >> 8) & 0xFF);
    out[trailer_pos + 2] = (uint8_t)((crc >> 16) & 0xFF);
    out[trailer_pos + 3] = (uint8_t)((crc >> 24) & 0xFF);
    out[trailer_pos + 4] = (uint8_t)(isize & 0xFF);
    out[trailer_pos + 5] = (uint8_t)((isize >> 8) & 0xFF);
    out[trailer_pos + 6] = (uint8_t)((isize >> 16) & 0xFF);
    out[trailer_pos + 7] = (uint8_t)((isize >> 24) & 0xFF);

    ObjString* result = obj_string_new((const char*)out, (uint32_t)total);
    free(out);
    return OBJ_VAL(result);
}

// ---- compress.gunzip(string) ----

static Value native_compress_gunzip(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ObjString* input = AS_STRING(args[0]);

    const uint8_t* data = (const uint8_t*)input->data;
    size_t len = input->length;

    // Validate gzip header
    if (len < 18) return VAL_NIL; // minimum gzip size
    if (data[0] != 0x1F || data[1] != 0x8B) return VAL_NIL; // magic
    if (data[2] != 0x08) return VAL_NIL; // must be deflate

    // Skip header (10 bytes for minimal header)
    size_t header_end = 10;
    uint8_t flags = data[3];

    // Handle FEXTRA
    if (flags & 0x04) {
        if (header_end + 2 > len) return VAL_NIL;
        uint16_t xlen = (uint16_t)(data[header_end] | (data[header_end + 1] << 8));
        header_end += 2 + xlen;
    }
    // Handle FNAME
    if (flags & 0x08) {
        while (header_end < len && data[header_end] != 0) header_end++;
        header_end++; // skip null terminator
    }
    // Handle FCOMMENT
    if (flags & 0x10) {
        while (header_end < len && data[header_end] != 0) header_end++;
        header_end++; // skip null terminator
    }
    // Handle FHCRC
    if (flags & 0x02) {
        header_end += 2;
    }

    if (header_end + 8 > len) return VAL_NIL;

    // Deflate data is between header and the last 8 bytes (trailer)
    size_t deflate_len = len - header_end - 8;

    size_t out_len;
    uint8_t* decompressed = inflate_stored(data + header_end, deflate_len, &out_len);
    if (!decompressed) return VAL_NIL;

    ObjString* result = obj_string_new((const char*)decompressed, (uint32_t)out_len);
    free(decompressed);
    return OBJ_VAL(result);
}

// ---- compress.encode_response(body, accept_encoding) ----
// Returns map {body: string, encoding: string}

static Value native_compress_encode_response(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0]) || !IS_STRING(args[1])) return VAL_NIL;

    ObjString* body = AS_STRING(args[0]);
    ObjString* accept = AS_STRING(args[1]);

    ObjMap* result = obj_map_new();
    gc_push_root(OBJ_VAL(result));

    // Check if gzip is accepted
    bool accepts_gzip = false;
    for (uint32_t i = 0; i + 3 < accept->length; i++) {
        if (accept->data[i] == 'g' && accept->data[i+1] == 'z' &&
            accept->data[i+2] == 'i' && accept->data[i+3] == 'p') {
            accepts_gzip = true;
            break;
        }
    }

    ObjString* body_key = obj_string_new("body", 4);
    gc_push_root(OBJ_VAL(body_key));
    ObjString* enc_key = obj_string_new("encoding", 8);
    gc_push_root(OBJ_VAL(enc_key));

    if (accepts_gzip) {
        // Compress with gzip
        size_t deflate_len;
        uint8_t* deflated = deflate_stored((const uint8_t*)body->data,
                                            body->length, &deflate_len);
        if (deflated) {
            size_t total = 10 + deflate_len + 8;
            uint8_t* out = (uint8_t*)malloc(total);
            if (out) {
                out[0] = 0x1F; out[1] = 0x8B; out[2] = 0x08; out[3] = 0x00;
                out[4] = 0x00; out[5] = 0x00; out[6] = 0x00; out[7] = 0x00;
                out[8] = 0x00; out[9] = 0xFF;
                memcpy(out + 10, deflated, deflate_len);

                uint32_t crc = crc32_compute((const uint8_t*)body->data, body->length);
                uint32_t isize = (uint32_t)(body->length & 0xFFFFFFFF);
                size_t tp = 10 + deflate_len;
                out[tp+0] = (uint8_t)(crc & 0xFF);
                out[tp+1] = (uint8_t)((crc >> 8) & 0xFF);
                out[tp+2] = (uint8_t)((crc >> 16) & 0xFF);
                out[tp+3] = (uint8_t)((crc >> 24) & 0xFF);
                out[tp+4] = (uint8_t)(isize & 0xFF);
                out[tp+5] = (uint8_t)((isize >> 8) & 0xFF);
                out[tp+6] = (uint8_t)((isize >> 16) & 0xFF);
                out[tp+7] = (uint8_t)((isize >> 24) & 0xFF);

                ObjString* compressed_body = obj_string_new((const char*)out, (uint32_t)total);
                obj_map_set(result, body_key, OBJ_VAL(compressed_body));
                free(out);
            }
            free(deflated);
        }
        ObjString* enc_val = obj_string_new("gzip", 4);
        obj_map_set(result, enc_key, OBJ_VAL(enc_val));
    } else {
        // No compression
        obj_map_set(result, body_key, OBJ_VAL(body));
        ObjString* enc_val = obj_string_new("identity", 8);
        obj_map_set(result, enc_key, OBJ_VAL(enc_val));
    }

    gc_pop_root(); // enc_key
    gc_pop_root(); // body_key
    gc_pop_root();
    return OBJ_VAL(result);
}

void stdlib_compress_init(ObjMap* pkg) {
    stdlib_register(pkg, "deflate", native_compress_deflate, 1);
    stdlib_register(pkg, "inflate", native_compress_inflate, 1);
    stdlib_register(pkg, "gzip", native_compress_gzip, 1);
    stdlib_register(pkg, "gunzip", native_compress_gunzip, 1);
    stdlib_register(pkg, "encode_response", native_compress_encode_response, 2);
}
