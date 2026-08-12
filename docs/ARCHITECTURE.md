# vek - System Architecture

> High-level architecture of the vek language runtime, compiler, VM, and deployment system.

---

## Overview

vek is a web-first language, runtime, and deployment system packaged as a single binary. The system consists of three main artifacts:

1. **`vek`** - The CLI + runtime. Compiles, runs, formats, REPL.
2. **`vekd`** - The dashboard/supervisor. Runs as a system service on the target VPS.
3. **`.vebc`** - The build artifact. Bytecode + assets + metadata.

---

## Compiler Pipeline

Source `.ve` files pass through a multi-stage compilation pipeline:

```
source .ve
  -> Tokenizer       (lexical analysis, produces token stream)
  -> Parser          (syntax analysis, produces AST)
  -> Resolver        (resolves free variables, identifies captures for closures)
  -> Type Checker    (annotated AST; advisory only in v1, no enforcement)
  -> Optimizer       (constant folding, dead branch elimination, inlining)
  -> Code Generator  (emits register-based bytecode)
  -> Packager        (assembles .vebc binary artifact)
```

### Key Design Decisions

- The pipeline is single-pass friendly: each stage feeds directly into the next.
- Type annotations are advisory in v1 - they guide opcode selection (typed vs generic) but do not produce compile-time errors.
- The optimizer's type inference pass is critical: it determines whether typed opcodes (`OP_ADD_INT`, `OP_ADD_FLOAT`) or generic opcodes (`OP_ADD`) are emitted, directly affecting runtime performance.
- `vek dev` keeps source around for hot-reload; `vek build` discards it.

---

## Virtual Machine

### Register-Based Architecture

The VM uses a register-based design rather than a stack machine. Each function has a fixed-size register window (up to 256 registers). Instructions name registers by small integer index.

**Advantages over stack machines:**
- Operands are explicit (no PUSH/POP pairs)
- Register allocation allows the C compiler to keep hot values in CPU registers
- One instruction does what would be 3-4 in a stack VM

**Example:** `a = b + c * d`
- Stack VM: `LOAD b, LOAD c, LOAD d, MUL, ADD, STORE a` (6 ops)
- Register VM: `MUL r_c, r_d, r_t1; ADD r_b, r_t1, r_a` (2 ops)

### NaN Boxing

All values are encoded in a single 64-bit `Value` using NaN-boxing:

| Top 16 bits | Meaning |
|---|---|
| `0x0000..0xFFFB` | IEEE 754 double (regular float) |
| `0x7FF8` | Pointer (low 47 bits; 16-byte aligned) |
| `0x7FF9` | Integer (low 48 bits, signed) |
| `0x7FFA_0000...` | `false` |
| `0x7FFA_8000...` | `true` |
| `0x7FFB_0000...` | `nil` |
| `0x7FFB_8000...` | `undefined` / unit |

**Key insight:** Doubles require no tag check for arithmetic. The "fast path" (no branch, no tag check) only holds when the compiler can prove operand types, which is why three separate opcodes exist: `OP_ADD_FLOAT`, `OP_ADD_INT`, and `OP_ADD` (generic with runtime tag check).

### Computed GOTO Dispatch

On GCC/Clang, the interpreter uses computed GOTO (indirect threading) for opcode dispatch:

```c
static const void *dispatch[] = {
  [OP_NOP] = &&op_nop,
  [OP_LOAD_CONST] = &&op_load_const,
  ...
};
#define DISPATCH() do { op = *ip++; goto *dispatch[op]; } while (0)
```

This compiles to a jump table + indirect branch. The CPU branch predictor learns the opcode stream after a few iterations, achieving approximately 1 op per cycle on modern x86. Falls back to `switch` on MSVC.

### Call Frames

```c
typedef struct {
  ObjClosure *closure;
  uint8_t    *ip;
  Value      *regs;       // points into function's register window
  uint16_t    reg_count;
} CallFrame;
```

- Frames live in a fixed-size array (default 256).
- Register windows allocated in a `Value regs[65536]` array.
- Tail calls reuse the current frame.
- Upvalues are closed when a function returns.

---

## Garbage Collector

### Mark-and-Sweep, Stop-the-World (v1)

Simple and reliable. No tuning knobs.

**Phases:**
1. **Stop the world** at a safepoint (function call, loop backedge, allocation over threshold).
2. **Mark roots**: globals, stack slots, upvalues, C-held handles.
3. **Mark transitively** following pointer values.
4. **Sweep** all heap pages: free unmarked, unmark marked.
5. **Resume**.

### Heap Layout

- Linked list of 16 KB pages.
- Bump allocation by default; free-list mode when a page fills.
- Each object starts with an `ObjHeader`:

```c
typedef struct {
  uint8_t  type;       // OBJ_STRING, OBJ_LIST, etc.
  uint8_t  flags;      // bit 0: mark, bit 1: pin, bit 2: large
  uint32_t size;       // total bytes including header
  uint32_t hash;       // for strings
  struct Page *page;   // back-pointer for sweep
} ObjHeader;
```

### Trigger Policy

- GC triggers when `bytes_allocated_since_gc > bytes_alive_after_last_gc * 2` (2x heap heuristic).
- Hard limit: 256 MB per app (configurable).

### What v1 Does Not Have

- No generational GC (v2)
- No incremental/concurrent GC (v2)
- No compacting GC
- No finalizers

---

## Concurrency Model

### Fibers + Worker Thread Pool

The model uses three pieces within one OS process:

1. **One main OS thread** - runs the VM interpreter, fiber scheduler, and event loop (epoll).
2. **A pool of N worker OS threads** - handles blocking I/O. Default `N = min(num_cores, 16)`.
3. **A set of fibers** - cooperative coroutines, one per in-flight unit of work.

```
  Main Thread                         Worker Thread Pool
  +--------------------------+        +-------------------+
  | Event Loop (epoll)       |        | T1: db.query()    |
  |   - I/O completions      |<------>| T2: http.get()    |
  |   - Timer fires          |  MPSC  | T3: file.read()   |
  |   - Accept new conns     |  ring  | T4: mail.send()   |
  |                          |        +-------------------+
  | Fiber Scheduler (FIFO)   |
  |   - Pop ready fiber      |
  |   - Resume until suspend |
  +--------------------------+
```

### How Blocking I/O Works

User code looks synchronous:
```ruby
rows = db.query("select * from posts where user_id = ?", uid)
```

Under the hood:
1. `OP_CALL db.query` dispatches to C built-in
2. Built-in marshals args into a heap-allocated `Job`
3. Job pushed onto worker pool's MPSC ring buffer
4. Built-in returns `VM_SUSPEND`
5. Fiber saved and parked; scheduler picks next ready fiber
6. Worker thread completes query, signals main thread via `eventfd`
7. Main thread wakes, marks fiber as READY
8. Fiber resumed, result materialized as normal `Value`

**Cost:** ~200 ns per fiber suspend/resume pair, invisible to user code.

### What Runs Where

| Operation | Thread |
|---|---|
| All user bytecode | Main |
| View rendering | Main |
| `db.query` / `db.exec` | Worker |
| `http.get` / `http.post` | Worker |
| `storage.get` / `storage.put` | Worker |
| `mail.send` | Worker |
| `json.encode` / `json.decode` | Main |
| `kv.*` / `cache.*` | Main (in-memory) |
| `crypto.sha256` / `uuid.v4` | Main (< 100 us) |
| `crypto.pbkdf2` | Worker (slow by design) |

### Multi-Core

To use multiple cores, run multiple vek processes for the same app. vekd's reverse proxy load-balances across them. No shared in-memory state across processes; use `db` for coordination.

### I/O Backends

- **Linux:** epoll, edge-triggered (direct implementation, no libuv)
- **macOS/BSD:** kqueue (for development)
- **No Windows** in v1

---

## Data Structures

### Strings (ObjString)
- Immutable, UTF-8, interned when identifier-like
- Length-prefixed: `s.length` is O(1)
- No ropes in v1; concatenation allocates new string

### Lists (ObjList)
- Dynamic array, doubles on growth
- O(1) amortized append, O(1) random access

### Maps (ObjMap)
- Open-addressing with linear probing
- String keys only in v1
- Insertion-ordered
- SmallMap optimization: maps with 4 or fewer entries use flat arrays

### Bytes (ObjBytes)
- Raw byte buffers for binary data (uploads, crypto, S3 blobs)

---

## Deployment Architecture

```
VPS (one machine, one systemd service)
+-----------------------------------------------+
|  vekd (:80/:443, :8080 UI)                    |
|    - Reverse proxy (host-header routing)       |
|    - Process supervisor                        |
|    - cgroup manager                            |
|    - Cloudflare API client                     |
|                                                |
|  App processes (each in own cgroup + user)     |
|    - vek app 1 (port 10001)                    |
|    - vek app 2 (port 10002)                    |
|    - ...                                       |
+-----------------------------------------------+
         |
         | cloudflared (optional tunnel)
         v
    Cloudflare (DNS, cache, TLS, DDoS)
         |
         v
      Internet
```

### Security Boundaries

- Each app runs as its own system user (`vek_<appname>`)
- Each app in its own cgroup v2 (memory, CPU, PID limits)
- Apps bind to `127.0.0.1`; only vekd listens publicly
- Env vars in 0600 files owned by the app user
- Secrets encrypted at rest with master key

### Deploy Flow

1. vekd clones/pulls the repo
2. Runs `vek build` to produce `.vebc`
3. Places artifact in a timestamped release directory
4. Starts the app via `systemd-run` with cgroup constraints
5. Waits for `/__ready__` health check
6. Updates reverse proxy routing table
7. Optionally purges Cloudflare cache

---

## Performance Targets

| Operation | Target |
|---|---|
| Cold start of vek binary | < 50 ms |
| Cold start of app | < 200 ms |
| HTTP request, no DB, simple render | < 1 ms |
| HTTP request, 1 DB query, render | < 5 ms |
| GC pause, 100 MB heap | < 20 ms |
| Build of 5000-line app | < 5 s |
| Memory per app, idle | < 30 MB |
