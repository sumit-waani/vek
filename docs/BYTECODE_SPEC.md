# vek - Bytecode Specification (.vebc)

> Binary format and opcode reference for the vek bytecode artifact.

---

## File Format Overview

A `.vebc` file is a single binary artifact, mmap-able, containing all compiled code and embedded assets for one vek application.

Sections appear in this fixed order:

1. Header
2. Constants Table
3. Strings Table
4. Function Table
5. Upvalue Table
6. Instruction Section
7. Line Table
8. Asset Section

---

## Header (64 bytes)

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 4 | `magic` | `"VEBC"` (0x56 0x45 0x42 0x43) |
| 4 | 2 | `version` | Format version (= 1 for v1) |
| 6 | 2 | `flags` | Bit flags (reserved in v1) |
| 8 | 32 | `sha256` | SHA-256 hash of all bytes after the header |
| 40 | 24 | `reserved` | Reserved for future use (zero-filled) |

**Verification:** On load, `vek run` computes SHA-256 of bytes 64..EOF and compares with the header's `sha256` field. Mismatch rejects the artifact.

**Reproducibility:** `vek build` zeroes all timestamps. Given the same source and compiler version, the output is byte-for-byte identical.

---

## Constants Table

Immediately follows the header.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 4 | `count` | Number of constants (u32, little-endian) |

Followed by `count` entries, each:

| Size | Field | Description |
|---|---|---|
| 1 | `tag` | Constant type (see below) |
| variable | `payload` | Type-dependent data |

### Constant Tags

| Tag | Value | Payload |
|---|---|---|
| `CONST_INT` | 0x01 | 8 bytes (i64, sign-extended from 48-bit) |
| `CONST_FLOAT` | 0x02 | 8 bytes (IEEE 754 double) |
| `CONST_STRING` | 0x03 | 4 bytes (u32 index into strings table) |
| `CONST_BYTES` | 0x04 | 4 bytes length + N bytes raw data |
| `CONST_FUNC_REF` | 0x05 | 4 bytes (u32 index into function table) |

---

## Strings Table

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 4 | `count` | Number of strings (u32) |

Followed by `count` entries:

| Size | Field | Description |
|---|---|---|
| 4 | `length` | Byte length of string (u32) |
| `length` | `data` | UTF-8 encoded bytes (no null terminator) |

Strings are referenced by index throughout the bytecode (function names, field names, global names, constant strings).

---

## Function Table

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 4 | `count` | Number of functions (u32) |

Followed by `count` entries (each 24 bytes):

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 4 | `name_idx` | Index into strings table (function name) |
| 4 | 2 | `num_regs` | Number of registers this function uses (u16) |
| 6 | 1 | `num_params` | Number of parameters (u8, max 255) |
| 7 | 1 | `num_upvalues` | Number of upvalues captured (u8) |
| 8 | 4 | `code_offset` | Byte offset into instruction section (u32) |
| 12 | 4 | `code_length` | Length of this function's bytecode in bytes (u32) |
| 16 | 4 | `line_table_offset` | Byte offset into line table section (u32) |
| 20 | 4 | `line_table_length` | Number of line table entries for this function (u32) |

Function index 0 is always the top-level/entry function.

---

## Upvalue Table

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 4 | `count` | Number of upvalue descriptors (u32) |

Followed by `count` entries (each 6 bytes):

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 4 | `src_idx` | Function index this upvalue came from (u32) |
| 4 | 1 | `slot` | Register slot in the source function (u8) |
| 5 | 1 | `is_local` | 1 if captures a local; 0 if captures another upvalue (u8) |

When creating a closure, the VM reads the upvalue descriptors to know which values to capture and whether they are direct locals or transitively captured upvalues.

---

## Instruction Section

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 4 | `total_length` | Total byte length of all instructions (u32) |

Followed by `total_length` bytes of raw bytecode. Instructions are variable-length: each starts with a 1-byte opcode followed by operands specific to that opcode.

---

## Line Table

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 4 | `count` | Total number of line table entries (u32) |

Followed by `count` entries (each 8 bytes):

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 4 | `code_offset` | Byte offset into instruction section (u32) |
| 4 | 4 | `source_line` | Source line number (1-based, u32) |

Entries are sorted by `code_offset`. To find the source line for an instruction, binary search for the largest `code_offset <= target`.

Used for error messages and stack traces. Stripped by default in release builds (included with `--with-lines`).

---

## Asset Section

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 4 | `count` | Number of embedded assets (u32) |

Followed by `count` entries:

| Size | Field | Description |
|---|---|---|
| 4 | `path` | Index into strings table (relative path, e.g., "public/style.css") |
| 4 | `length` | Byte length of asset data (u32) |
| `length` | `data` | Raw asset bytes |

Assets are the contents of the `public/` directory, embedded at build time for single-file deployment. The HTTP server serves them directly from the mmap'd .vebc file.

---

## Instruction Encoding

All instructions start with a 1-byte opcode. Operand layouts vary by instruction:

### Encoding Formats

| Format | Layout | Size | Description |
|---|---|---|---|
| `N` | `[op]` | 1 byte | No operands |
| `A` | `[op][ra]` | 2 bytes | Single register |
| `AB` | `[op][ra][rb]` | 3 bytes | Two registers |
| `ABC` | `[op][ra][rb][rc]` | 4 bytes | Three registers |
| `AI32` | `[op][ra][imm32]` | 6 bytes | Register + 32-bit immediate |
| `AI16` | `[op][ra][imm16]` | 4 bytes | Register + 16-bit offset |
| `I16` | `[op][imm16]` | 3 bytes | 16-bit signed offset (jumps) |
| `AU32` | `[op][ra][u32]` | 6 bytes | Register + 32-bit unsigned index |

Registers are 1-byte unsigned indices (0-255).  
Immediates are little-endian.  
Jump offsets are signed 16-bit, relative to the instruction following the jump.

---

## Opcode Reference

### Miscellaneous

| Opcode | Value | Format | Description |
|---|---|---|---|
| `OP_NOP` | 0x00 | N | No operation |
| `OP_HALT` | 0x01 | N | Stop execution |

### Load/Store

| Opcode | Value | Format | Description |
|---|---|---|---|
| `OP_MOVE` | 0x02 | AB | `r[A] = r[B]` |
| `OP_LOAD_CONST` | 0x03 | AU32 | `r[A] = constants[u32]` |
| `OP_LOAD_NIL` | 0x04 | A | `r[A] = nil` |
| `OP_LOAD_TRUE` | 0x05 | A | `r[A] = true` |
| `OP_LOAD_FALSE` | 0x06 | A | `r[A] = false` |
| `OP_LOAD_INT` | 0x07 | AI32 | `r[A] = (int)imm32` (small int literal) |
| `OP_LOAD_FLOAT` | 0x08 | AU32 | `r[A] = constants[u32]` (float from const table) |
| `OP_LOAD_GLOBAL` | 0x09 | AU32 | `r[A] = globals[name_idx]` |
| `OP_STORE_GLOBAL` | 0x0A | AU32 | `globals[name_idx] = r[A]` |
| `OP_GET_LOCAL` | 0x0B | AB | `r[A] = locals[B]` |
| `OP_SET_LOCAL` | 0x0C | AB | `locals[A] = r[B]` |

### Field/Index Access

| Opcode | Value | Format | Description |
|---|---|---|---|
| `OP_GET_FIELD` | 0x0D | ABC | `r[A] = r[B].fields[name_idx(C)]` |
| `OP_SET_FIELD` | 0x0E | ABC | `r[A].fields[name_idx(B)] = r[C]` |
| `OP_GET_INDEX` | 0x0F | ABC | `r[A] = r[B][r[C]]` |
| `OP_SET_INDEX` | 0x10 | ABC | `r[A][r[B]] = r[C]` |

### Arithmetic (Generic)

| Opcode | Value | Format | Description |
|---|---|---|---|
| `OP_ADD` | 0x11 | ABC | `r[A] = r[B] + r[C]` (runtime type check) |
| `OP_SUB` | 0x12 | ABC | `r[A] = r[B] - r[C]` |
| `OP_MUL` | 0x13 | ABC | `r[A] = r[B] * r[C]` |
| `OP_DIV` | 0x14 | ABC | `r[A] = r[B] / r[C]` (always returns float) |
| `OP_MOD` | 0x15 | ABC | `r[A] = r[B] % r[C]` |
| `OP_NEG` | 0x16 | AB | `r[A] = -r[B]` |

### Arithmetic (Typed - Fast Path)

| Opcode | Value | Format | Description |
|---|---|---|---|
| `OP_ADD_INT` | 0x17 | ABC | `r[A] = r[B] + r[C]` (no tag check, int) |
| `OP_SUB_INT` | 0x18 | ABC | `r[A] = r[B] - r[C]` (int) |
| `OP_MUL_INT` | 0x19 | ABC | `r[A] = r[B] * r[C]` (int) |
| `OP_ADD_FLOAT` | 0x1A | ABC | `r[A] = r[B] + r[C]` (no tag check, float) |
| `OP_SUB_FLOAT` | 0x1B | ABC | `r[A] = r[B] - r[C]` (float) |
| `OP_MUL_FLOAT` | 0x1C | ABC | `r[A] = r[B] * r[C]` (float) |
| `OP_DIV_FLOAT` | 0x1D | ABC | `r[A] = r[B] / r[C]` (float) |

### Comparison

| Opcode | Value | Format | Description |
|---|---|---|---|
| `OP_EQ` | 0x1E | ABC | `r[A] = (r[B] == r[C])` |
| `OP_NEQ` | 0x1F | ABC | `r[A] = (r[B] != r[C])` |
| `OP_LT` | 0x20 | ABC | `r[A] = (r[B] < r[C])` |
| `OP_LTE` | 0x21 | ABC | `r[A] = (r[B] <= r[C])` |
| `OP_GT` | 0x22 | ABC | `r[A] = (r[B] > r[C])` |
| `OP_GTE` | 0x23 | ABC | `r[A] = (r[B] >= r[C])` |

### Logical/Bitwise

| Opcode | Value | Format | Description |
|---|---|---|---|
| `OP_NOT` | 0x24 | AB | `r[A] = !r[B]` (truthy/falsy inversion) |
| `OP_BAND` | 0x25 | ABC | `r[A] = r[B] & r[C]` (bitwise AND) |
| `OP_BOR` | 0x26 | ABC | `r[A] = r[B] | r[C]` (bitwise OR) |
| `OP_BXOR` | 0x27 | ABC | `r[A] = r[B] ^ r[C]` (bitwise XOR) |
| `OP_BNOT` | 0x28 | AB | `r[A] = ~r[B]` (bitwise NOT) |
| `OP_SHL` | 0x29 | ABC | `r[A] = r[B] << r[C]` (shift left) |
| `OP_SHR` | 0x2A | ABC | `r[A] = r[B] >> r[C]` (shift right) |

### Control Flow

| Opcode | Value | Format | Description |
|---|---|---|---|
| `OP_JUMP` | 0x2B | I16 | `ip += offset` (unconditional) |
| `OP_JUMP_IF_FALSE` | 0x2C | AI16 | `if !truthy(r[A]) ip += offset` |
| `OP_JUMP_IF_TRUE` | 0x2D | AI16 | `if truthy(r[A]) ip += offset` |

### Function Calls

| Opcode | Value | Format | Description |
|---|---|---|---|
| `OP_CALL` | 0x2E | ABC | Call `r[A]` with `B` args starting at `r[A+1]`, result in `r[C]` |
| `OP_TAILCALL` | 0x2F | AB | Tail-call `r[A]` with `B` args (reuses frame) |
| `OP_RETURN` | 0x30 | A | Return `r[A]` from current function |

### Closures and Upvalues

| Opcode | Value | Format | Description |
|---|---|---|---|
| `OP_CLOSURE` | 0x31 | AU32 | `r[A] = new_closure(func_table[u32])` |
| `OP_GET_UPVALUE` | 0x32 | AB | `r[A] = upvalues[B]` |
| `OP_SET_UPVALUE` | 0x33 | AB | `upvalues[A] = r[B]` |
| `OP_CLOSE_UPVALUE` | 0x34 | A | Close upvalue at register `A` (promote to heap) |

### Object Construction

| Opcode | Value | Format | Description |
|---|---|---|---|
| `OP_NEW_LIST` | 0x35 | AB | `r[A] = new list` from `B` values starting at `r[A+1]` |
| `OP_NEW_MAP` | 0x36 | AB | `r[A] = new map` from `B` key-value pairs |
| `OP_NEW_BYTES` | 0x37 | AB | `r[A] = new bytes` from `B` values |

### Iteration

| Opcode | Value | Format | Description |
|---|---|---|---|
| `OP_FOR_PREP` | 0x38 | AI16 | Prepare numeric range iterator in `r[A]`, jump `offset` if empty |
| `OP_FOR_NEXT` | 0x39 | AI16 | Advance range iterator `r[A]`, jump `offset` when exhausted |
| `OP_ITER_NEW` | 0x3A | AB | `r[A] = new_iterator(r[B])` (list/map iter) |
| `OP_ITER_NEXT` | 0x3B | ABC | `r[A], r[B] = next(iter_r[C])`, jump if done |

### Error Handling

| Opcode | Value | Format | Description |
|---|---|---|---|
| `OP_THROW` | 0x3C | A | `raise r[A]` (unwind to nearest handler) |
| `OP_PUSH_HANDLER` | 0x3D | I16 | Push rescue handler at `ip + offset` |
| `OP_POP_HANDLER` | 0x3E | N | Pop the topmost rescue handler |

### Miscellaneous

| Opcode | Value | Format | Description |
|---|---|---|---|
| `OP_DUP` | 0x3F | AB | `r[A] = r[B]` (alias for MOVE, semantic clarity) |
| `OP_POP` | 0x40 | A | Discard `r[A]` (no-op in register VM, used for clarity) |
| `OP_IMPORT` | 0x41 | AU32 | Resolve stdlib package `strings[u32]` (no-op if loaded) |
| `OP_CONCAT` | 0x42 | ABC | `r[A] = concat(r[B]..r[B+C-1])` (string concatenation) |
| `OP_RANGE` | 0x43 | ABC | `r[A] = range(r[B], r[C])` (inclusive) |
| `OP_RANGE_EX` | 0x44 | ABC | `r[A] = range(r[B], r[C])` (exclusive) |

---

## Opcode Encoding Summary

Total: approximately 60 opcodes (0x00 through 0x44 = 69 opcodes defined above; actual count may vary slightly during implementation).

### Opcode Space

- Values 0x00-0x7F: reserved for v1 opcodes
- Values 0x80-0xFF: reserved for future expansion (v2+)

### Operand Conventions

- `ra`, `rb`, `rc` - register indices (0-255, 1 byte each)
- `imm16` - signed 16-bit immediate (little-endian)
- `imm32` - signed 32-bit immediate (little-endian)
- `u32` - unsigned 32-bit index (little-endian)

---

## Dispatch Implementation

### Computed GOTO (GCC/Clang)

```c
static const void *dispatch[] = {
  [OP_NOP]        = &&op_nop,
  [OP_HALT]       = &&op_halt,
  [OP_MOVE]       = &&op_move,
  [OP_LOAD_CONST] = &&op_load_const,
  // ...
};

#define DISPATCH() do { op = *ip++; goto *dispatch[op]; } while (0)

uint8_t op;
DISPATCH();

op_nop:
  DISPATCH();

op_move:
  r[ip[0]] = r[ip[1]];
  ip += 2;
  DISPATCH();

op_load_const:
  r[ip[0]] = vm->constants[read_u32(ip + 1)];
  ip += 5;
  DISPATCH();
// ...
```

### Switch Fallback (MSVC/portable)

```c
for (;;) {
  uint8_t op = *ip++;
  switch (op) {
    case OP_NOP: break;
    case OP_MOVE: r[ip[0]] = r[ip[1]]; ip += 2; break;
    case OP_LOAD_CONST: r[ip[0]] = vm->constants[read_u32(ip+1)]; ip += 5; break;
    // ...
  }
}
```

---

## Size Estimates

| App Type | Approximate .vebc Size |
|---|---|
| Hello world | ~2-5 KB |
| Minimal web app | ~20-50 KB |
| Medium app (blog) | ~100-500 KB |
| Large app with vendored assets | < 1 MB |

---

## Versioning

The `version` field in the header identifies the bytecode format version. The runtime refuses to load artifacts with a version it does not support. Version increments indicate breaking changes to the instruction encoding, section layout, or opcode semantics.

| Version | Description |
|---|---|
| 1 | Initial v1 format (this document) |
