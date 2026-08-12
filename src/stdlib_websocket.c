#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

// External base64_encode from stdlib_session.c
extern char* base64_encode(const uint8_t* data, size_t len, size_t* out_len);

// ---- Minimal SHA-1 implementation (for WebSocket handshake) ----

#define SHA1_ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xEFCDAB89;
    uint32_t h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476;
    uint32_t h4 = 0xC3D2E1F0;

    // Pre-processing: pad message
    size_t padded_len = ((len + 8) / 64 + 1) * 64;
    uint8_t* msg = (uint8_t*)calloc(padded_len, 1);
    memcpy(msg, data, len);
    msg[len] = 0x80;

    uint64_t bit_len = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) {
        msg[padded_len - 1 - i] = (uint8_t)(bit_len >> (i * 8));
    }

    // Process blocks
    for (size_t offset = 0; offset < padded_len; offset += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)msg[offset + i*4] << 24) |
                   ((uint32_t)msg[offset + i*4+1] << 16) |
                   ((uint32_t)msg[offset + i*4+2] << 8) |
                   ((uint32_t)msg[offset + i*4+3]);
        }
        for (int i = 16; i < 80; i++) {
            w[i] = SHA1_ROTL(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        }

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;

        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }

            uint32_t temp = SHA1_ROTL(a, 5) + f + e + k + w[i];
            e = d; d = c; c = SHA1_ROTL(b, 30); b = a; a = temp;
        }

        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    free(msg);

    // Output hash
    out[0]  = (uint8_t)(h0 >> 24); out[1]  = (uint8_t)(h0 >> 16);
    out[2]  = (uint8_t)(h0 >> 8);  out[3]  = (uint8_t)(h0);
    out[4]  = (uint8_t)(h1 >> 24); out[5]  = (uint8_t)(h1 >> 16);
    out[6]  = (uint8_t)(h1 >> 8);  out[7]  = (uint8_t)(h1);
    out[8]  = (uint8_t)(h2 >> 24); out[9]  = (uint8_t)(h2 >> 16);
    out[10] = (uint8_t)(h2 >> 8);  out[11] = (uint8_t)(h2);
    out[12] = (uint8_t)(h3 >> 24); out[13] = (uint8_t)(h3 >> 16);
    out[14] = (uint8_t)(h3 >> 8);  out[15] = (uint8_t)(h3);
    out[16] = (uint8_t)(h4 >> 24); out[17] = (uint8_t)(h4 >> 16);
    out[18] = (uint8_t)(h4 >> 8);  out[19] = (uint8_t)(h4);
}

// ---- WebSocket frame encoding/decoding (RFC 6455) ----

// ws.encode_frame(payload, opcode)
// Encode a WebSocket frame. Server frames are NOT masked.
// Returns a string containing the raw frame bytes.
static Value native_ws_encode_frame(int argc, Value* args) {
    (void)argc;
    if (!IS_STRING(args[0])) return VAL_NIL;

    ObjString* payload = AS_STRING(args[0]);
    int opcode = 1; // default: text frame
    if (argc >= 2 && IS_INT(args[1])) {
        opcode = (int)AS_INT(args[1]);
    }

    size_t payload_len = payload->length;
    size_t frame_size = 2 + payload_len; // base header + payload

    // Extended payload length
    if (payload_len >= 126 && payload_len <= 65535) {
        frame_size = 2 + 2 + payload_len;
    } else if (payload_len > 65535) {
        frame_size = 2 + 8 + payload_len;
    }

    uint8_t* frame = (uint8_t*)malloc(frame_size);
    size_t pos = 0;

    // First byte: FIN=1, opcode
    frame[pos++] = (uint8_t)(0x80 | (opcode & 0x0F));

    // Second byte: MASK=0, payload length
    if (payload_len < 126) {
        frame[pos++] = (uint8_t)payload_len;
    } else if (payload_len <= 65535) {
        frame[pos++] = 126;
        frame[pos++] = (uint8_t)(payload_len >> 8);
        frame[pos++] = (uint8_t)(payload_len & 0xFF);
    } else {
        frame[pos++] = 127;
        for (int i = 7; i >= 0; i--) {
            frame[pos++] = (uint8_t)((payload_len >> (i * 8)) & 0xFF);
        }
    }

    // Payload (no masking for server frames)
    memcpy(frame + pos, payload->data, payload_len);
    pos += payload_len;

    ObjString* result = obj_string_new((const char*)frame, (uint32_t)pos);
    free(frame);
    return OBJ_VAL(result);
}

// ws.decode_frame(data)
// Decode a WebSocket frame. Returns map {opcode: int, payload: string, fin: bool}
// or nil if data is incomplete.
static Value native_ws_decode_frame(int argc, Value* args) {
    (void)argc;
    if (!IS_STRING(args[0])) return VAL_NIL;

    ObjString* data = AS_STRING(args[0]);
    const uint8_t* buf = (const uint8_t*)data->data;
    size_t buf_len = data->length;

    if (buf_len < 2) return VAL_NIL;

    size_t pos = 0;

    // First byte
    uint8_t byte0 = buf[pos++];
    bool fin = (byte0 & 0x80) != 0;
    int opcode = byte0 & 0x0F;

    // Second byte
    uint8_t byte1 = buf[pos++];
    bool masked = (byte1 & 0x80) != 0;
    uint64_t payload_len = byte1 & 0x7F;

    if (payload_len == 126) {
        if (buf_len < pos + 2) return VAL_NIL;
        payload_len = ((uint64_t)buf[pos] << 8) | buf[pos + 1];
        pos += 2;
    } else if (payload_len == 127) {
        if (buf_len < pos + 8) return VAL_NIL;
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | buf[pos + i];
        }
        pos += 8;
    }

    // Masking key
    uint8_t mask_key[4] = {0};
    if (masked) {
        if (buf_len < pos + 4) return VAL_NIL;
        memcpy(mask_key, buf + pos, 4);
        pos += 4;
    }

    // Check we have enough data
    if (buf_len < pos + payload_len) return VAL_NIL;

    // Extract and unmask payload
    char* payload = (char*)malloc(payload_len + 1);
    memcpy(payload, buf + pos, payload_len);
    if (masked) {
        for (uint64_t i = 0; i < payload_len; i++) {
            payload[i] ^= (char)mask_key[i % 4];
        }
    }
    payload[payload_len] = '\0';

    // Build result map
    ObjString* payload_str = obj_string_new(payload, (uint32_t)payload_len);
    gc_push_root(OBJ_VAL(payload_str));
    free(payload);

    ObjMap* result = obj_map_new();
    gc_push_root(OBJ_VAL(result));

    ObjString* k_opcode = obj_string_new("opcode", 6);
    gc_push_root(OBJ_VAL(k_opcode));
    obj_map_set(result, k_opcode, INT_VAL(opcode));
    gc_pop_root();

    ObjString* k_payload = obj_string_new("payload", 7);
    gc_push_root(OBJ_VAL(k_payload));
    obj_map_set(result, k_payload, OBJ_VAL(payload_str));
    gc_pop_root();

    ObjString* k_fin = obj_string_new("fin", 3);
    gc_push_root(OBJ_VAL(k_fin));
    obj_map_set(result, k_fin, fin ? VAL_TRUE : VAL_FALSE);
    gc_pop_root();

    gc_pop_root(); // result
    gc_pop_root(); // payload_str

    return OBJ_VAL(result);
}

// ws.accept_key(client_key)
// Compute Sec-WebSocket-Accept: SHA1(key + GUID), base64 encoded
static Value native_ws_accept_key(int argc, Value* args) {
    (void)argc;
    if (!IS_STRING(args[0])) return VAL_NIL;

    ObjString* client_key = AS_STRING(args[0]);
    static const char* ws_guid = "258EAFA5-E914-47DA-95CA-5AB9ADF635B0";
    size_t guid_len = 36;

    // Concatenate key + GUID
    size_t concat_len = client_key->length + guid_len;
    char* concat = (char*)malloc(concat_len + 1);
    memcpy(concat, client_key->data, client_key->length);
    memcpy(concat + client_key->length, ws_guid, guid_len);
    concat[concat_len] = '\0';

    // SHA-1 hash
    uint8_t hash[20];
    sha1((const uint8_t*)concat, concat_len, hash);
    free(concat);

    // Base64 encode
    size_t b64_len = 0;
    char* b64 = base64_encode(hash, 20, &b64_len);

    ObjString* result = obj_string_new(b64, (uint32_t)b64_len);
    free(b64);
    return OBJ_VAL(result);
}

// ws.upgrade_headers(request_key)
// Return map of response headers for WebSocket upgrade
static Value native_ws_upgrade_headers(int argc, Value* args) {
    (void)argc;
    if (!IS_STRING(args[0])) return VAL_NIL;

    // Compute accept key
    ObjString* client_key = AS_STRING(args[0]);
    static const char* ws_guid = "258EAFA5-E914-47DA-95CA-5AB9ADF635B0";
    size_t guid_len = 36;

    size_t concat_len = client_key->length + guid_len;
    char* concat = (char*)malloc(concat_len + 1);
    memcpy(concat, client_key->data, client_key->length);
    memcpy(concat + client_key->length, ws_guid, guid_len);
    concat[concat_len] = '\0';

    uint8_t hash[20];
    sha1((const uint8_t*)concat, concat_len, hash);
    free(concat);

    size_t b64_len = 0;
    char* b64 = base64_encode(hash, 20, &b64_len);

    // Build headers map
    ObjMap* headers = obj_map_new();
    gc_push_root(OBJ_VAL(headers));

    ObjString* k1 = obj_string_new("Upgrade", 7);
    gc_push_root(OBJ_VAL(k1));
    ObjString* v1 = obj_string_new("websocket", 9);
    gc_push_root(OBJ_VAL(v1));
    obj_map_set(headers, k1, OBJ_VAL(v1));
    gc_pop_root(); gc_pop_root();

    ObjString* k2 = obj_string_new("Connection", 10);
    gc_push_root(OBJ_VAL(k2));
    ObjString* v2 = obj_string_new("Upgrade", 7);
    gc_push_root(OBJ_VAL(v2));
    obj_map_set(headers, k2, OBJ_VAL(v2));
    gc_pop_root(); gc_pop_root();

    ObjString* k3 = obj_string_new("Sec-WebSocket-Accept", 20);
    gc_push_root(OBJ_VAL(k3));
    ObjString* v3 = obj_string_new(b64, (uint32_t)b64_len);
    gc_push_root(OBJ_VAL(v3));
    obj_map_set(headers, k3, OBJ_VAL(v3));
    gc_pop_root(); gc_pop_root();

    free(b64);
    gc_pop_root(); // headers

    return OBJ_VAL(headers);
}

void stdlib_websocket_init(ObjMap* pkg) {
    stdlib_register(pkg, "encode_frame", native_ws_encode_frame, -1);
    stdlib_register(pkg, "decode_frame", native_ws_decode_frame, 1);
    stdlib_register(pkg, "accept_key", native_ws_accept_key, 1);
    stdlib_register(pkg, "upgrade_headers", native_ws_upgrade_headers, 1);
}
