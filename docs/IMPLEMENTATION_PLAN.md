# vek - Implementation Plan

> Phased roadmap for implementing the vek language, runtime, and deployment system.

---

## Guiding Principles

- Each phase produces a working, testable system.
- Dependencies flow strictly downward: later phases build on earlier ones.
- The core language runtime (Phase 1) is the foundation everything else depends on.
- Success criteria are concrete and measurable.

---

## Phase 1: Core Language Runtime

**Goal:** A working interpreter that can tokenize, parse, compile, and execute vek programs with all core language features.

### Deliverables

#### 1.1 Lexer/Tokenizer
- Token types: keywords, identifiers, literals (int, float, string, symbol), operators, delimiters
- String interpolation detection (`"hello #{expr}"`)
- Symbol literals (`:foo`)
- Line tracking for error reporting
- Single-pass, streaming tokenization

#### 1.2 Parser
- Recursive descent parser producing an AST
- All control flow: `if/elsif/else/end`, `while`, `until`, `loop do/end`, `for..in`, `case..in`
- Function definitions with optional type annotations
- Lambda/block syntax: `->() {}`, `do |x| ... end`
- Operator precedence (arithmetic, comparison, logical, bitwise)
- Expression-position `if` and `case`
- String interpolation desugaring to `concat`

#### 1.3 AST and Resolver
- AST node types for all language constructs
- Variable resolution: local, upvalue, global
- Free variable detection and closure capture analysis
- Constant detection (UPPERCASE identifiers)
- Scope tracking (file-scoped vs global from app.ve)

#### 1.4 Code Generator
- Emit register-based bytecode from resolved AST
- Register allocation (linear scan or simple graph coloring)
- Type inference pass for typed opcode selection (`OP_ADD_INT` vs `OP_ADD_FLOAT` vs `OP_ADD`)
- Constant folding and dead branch elimination
- Tail call detection
- Loop variable rebinding (fresh binding per `for` iteration)
- Upvalue emission for closures

#### 1.5 Virtual Machine
- NaN-boxed 64-bit `Value` type with encoding/decoding macros
- Register-based execution with computed GOTO dispatch (GCC/Clang)
- Switch-based fallback for other compilers
- Call frame stack (default 256 frames)
- Register window allocation (64K value array)
- All ~60 opcodes implemented
- Closure creation and upvalue management
- Tail call optimization
- Built-in function dispatch (`is_builtin` flag on call)

#### 1.6 Garbage Collector
- Mark-and-sweep, stop-the-world
- 16 KB page-based heap with bump allocation
- Object header: type, flags (mark/pin/large), size, hash
- Root scanning: globals, stack, upvalues, C-held handles
- Trigger: 2x heap heuristic
- Pin/unpin API for C code holding references

#### 1.7 Core Types
- `nil`, `bool`, `int` (48-bit signed), `float` (IEEE 754 double)
- `ObjString`: immutable, UTF-8, interned for identifier-like strings, hash cached
- `ObjList`: dynamic array, doubles on growth
- `ObjMap`: open-addressing, linear probing, string keys, insertion-ordered, SmallMap (4 or fewer entries)
- `ObjBytes`: raw byte buffer
- Methods on built-in types (string: `length`, `slice`, `upper`, etc.; list: `push`, `pop`, `map`, `filter`, etc.; map: `get`, `set`, `keys`, etc.)

#### 1.8 Error Handling
- `raise` with message string (allocates error object, walks call stack)
- `rescue` blocks (push/pop handler on handler stack)
- `Unwind` signal (non-catchable, longjmp to request handler boundary)
- `halt` and `redirect` as Unwind triggers

### Dependencies
- C11 compiler (clang 15 or gcc)
- Make build system
- No external libraries for the core runtime

### Success Criteria
- Can tokenize, parse, compile, and run vek programs
- All value types work correctly with NaN-boxing
- Closures capture variables correctly (including per-iteration rebinding in `for`)
- GC collects unreachable objects without corrupting live data
- Tail calls do not grow the frame stack
- Computed GOTO dispatch achieves measurable speedup over switch
- Basic REPL works (read, compile, execute, print)
- Test suite passes for: arithmetic, string ops, list/map ops, control flow, closures, GC stress

---

## Phase 2: Standard Library and HTTP Server

**Goal:** The 30 built-in packages, file-based routing, view rendering, and a working HTTP server.

### Deliverables

#### 2.1 Fiber Scheduler and I/O
- Fiber implementation (cooperative coroutines with 64 KB initial C stack)
- Fiber states: READY, RUNNING, SUSPENDED, DEAD
- Context switching (`makecontext`/`swapcontext` or hand-rolled assembly)
- FIFO run queue
- Event loop: epoll (Linux), kqueue (macOS)
- Worker thread pool (MPSC ring buffer for job dispatch)
- eventfd signaling from worker to main thread

#### 2.2 HTTP Server
- HTTP/1.1 parser (request line, headers, body)
- Keep-alive connection management
- File-based router (trie-based, supports dynamic segments, constraints, catch-all)
- Request object (`req.method`, `req.path`, `req.query`, `req.body`, `req.params`, `req.headers`, `req.session`, etc.)
- Response helpers: `render`, `json`, `text`, `redirect`, `halt`, `empty`, `file`
- Static file serving from `public/`
- Multipart form parsing and file uploads

#### 2.3 View System
- HTML DSL helpers (`h1`, `div`, `a`, `form`, etc.)
- Internal `Builder` type for O(n) string assembly
- Layout system (`layout "main.ve" do ... end`)
- Partials (`partial "_card.ve", data: x`)
- Auto-escaping of interpolated values
- `raw` helper for unescaped HTML

#### 2.4 Core Stdlib Packages
- `db` (SQLite with WAL mode, connection pool, parameterized queries, transactions)
- `kv` (in-memory LRU with TTL)
- `cache` (TTL cache with `get_or_set`)
- `json` (encode/decode)
- `form` (validation DSL)
- `session` (signed cookie sessions)
- `csrf` (token generation/validation)
- `auth` (bcrypt hash/verify, random_token)
- `log` (structured logging, text/JSON modes)
- `env` (env var access with defaults)
- `time` (now, format, parse, durations)
- `uuid` (v4, v7)
- `crypto` (sha256, hmac, random_bytes)
- `path` (URL path utilities)

#### 2.5 Extended Stdlib Packages
- `http` (HTTP client with timeouts, retries)
- `mail` (SMTP send)
- `jobs` (background queue with SQLite persistence, retry with backoff)
- `storage` (S3-compatible blob storage)
- `flash` (one-shot session messages)
- `ratelimit` (token bucket per key)
- `compress` (gzip + brotli)
- `websocket` (per-connection handlers)
- `i18n` (key-based translations)
- `webhook` (signature verification)
- `markdown` (markdown to HTML)
- `sanitize` (HTML allowlist)
- `csp` (Content-Security-Policy builder)
- `slug` (URL slugification)
- `cors` (CORS headers)
- `cli` (stdin, args, ANSI colors)

### Dependencies
- Phase 1 complete
- SQLite amalgamation (vendored)
- OpenSSL or a minimal TLS library for crypto primitives

### Success Criteria
- A complete vek web app (file-based routing, DB queries, views) runs and serves HTTP
- Fiber-based concurrency handles multiple concurrent requests without blocking
- All 30 stdlib packages have working implementations
- CSRF protection works on HTML forms, exempt for API routes
- GC works correctly under concurrent fiber load
- Performance: < 5 ms for a request with 1 DB query and view render

---

## Phase 3: CLI Tooling

**Goal:** The complete `vek` CLI: project scaffolding, dev server with hot reload, build, formatter, REPL, migrations, and test runner.

### Deliverables

#### 3.1 CLI Framework
- Subcommand routing (`vek new`, `vek dev`, `vek build`, etc.)
- Argument parsing, help text, version info
- ANSI color output for dev experience

#### 3.2 `vek new`
- Project scaffolding (directory structure, starter files)
- Interactive prompts (app name, db choice, starter template)

#### 3.3 `vek dev`
- File watcher (inotify on Linux, kqueue on macOS)
- Incremental recompilation of changed files
- Auto-restart on `app.ve` changes
- Pretty error pages with source position and stack trace
- Configurable port (`--port` / `PORT` env var)

#### 3.4 `vek build`
- Full pipeline execution: compile all `.ve` files
- `.vebc` packager (header, constants, strings, functions, upvalues, instructions, line table, assets)
- Asset embedding (all `public/` files into asset section)
- SHA-256 integrity hash
- Reproducible builds (timestamps zeroed)

#### 3.5 `vek run`
- Load and verify `.vebc` artifact
- Start HTTP server with configurable workers (multi-process)

#### 3.6 `vek fmt`
- Opinionated formatter (2-space indent, 100 char max line)
- In-place formatting of `.ve` files

#### 3.7 `vek repl` / `vek shell`
- Read-eval-print loop with multi-line input
- `repl`: bare language context
- `shell`: loads `app.ve`, has `db`/`kv`/etc. in scope

#### 3.8 `vek migrate`
- Apply pending SQL migrations from `migrations/`
- `status`, `new NAME` subcommands
- Migration tracking in `_migrations` table

#### 3.9 `vek test`
- Test file discovery (`*.test.ve`)
- Assertion helpers
- Test runner with pass/fail reporting

### Dependencies
- Phase 2 complete

### Success Criteria
- `vek new myapp && cd myapp && vek dev` produces a running app
- `vek build` produces a valid `.vebc` that `vek run` can execute
- `vek fmt` formats code idempotently
- `vek migrate` applies migrations in order and tracks state
- Hot reload detects file changes and recompiles within 100 ms
- REPL evaluates expressions and prints results

---

## Phase 4: vekd Dashboard and Deploy

**Goal:** The `vekd` supervisor binary - a web UI-only dashboard for managing production deployments. Users SSH once to install vekd, then manage everything via the web dashboard at `:8080`. No CLI interface beyond the initial install command.

### Deliverables

#### 4.1 vekd Core
- SQLite-based state (apps, releases, env_vars, events, secrets, users)
- Master key management (encryption at rest for secrets)
- systemd integration (`systemd-run` for app processes)
- Fallback supervisor (fork/exec + waitpid for non-systemd environments)

#### 4.2 Reverse Proxy
- Host-header-based routing to app ports
- Connection pool (4 persistent connections per app)
- `X-Forwarded-For`, `X-Forwarded-Proto`, `X-Real-IP` injection
- Timeouts: 30s read, 60s total, 5s connect
- Health check polling (`/__health__`, `/__ready__`)

#### 4.3 cgroup v2 Management
- Create per-app cgroups under `/sys/fs/cgroup/vek/`
- Set `memory.max`, `cpu.max`, `pids.max`
- Per-app system user creation and isolation

#### 4.4 Deploy Pipeline
- Git clone/pull with PAT authentication
- `vek build` execution
- Timestamped release directories
- Symlink-based current release
- Health check with 30s timeout
- Automatic rollback on failed health check

#### 4.5 Web UI (Primary Interface)
- HTML + htmx (no client-side framework)
- Login, dashboard, app management pages
- Deploy trigger, log tailing, env var management
- System resource monitoring
- App creation, deletion, and configuration
- User management (add/remove users, role assignment)
- Backup and restore operations
- Cloudflare DNS and tunnel management
- All operations that would traditionally require a CLI are performed here

#### 4.6 Cloudflare Integration
- API token management (encrypted storage)
- DNS record creation/update/deletion
- Cache purge on deploy
- Optional cloudflared tunnel management

### Dependencies
- Phase 3 complete
- Target Linux with cgroup v2 and systemd

### Success Criteria
- A fresh VPS can be set up with one `curl | sh` command
- Apps deploy from git repos via the web dashboard
- Apps are isolated (cannot read each other's data)
- Crashed apps are automatically restarted
- Rollback works within seconds
- Cloudflare DNS is configured automatically
- vekd web UI at :8080 is the sole management interface for all operations
- The deployment workflow is: SSH once to install, then manage everything via web UI

---

## Implementation Order Within Phase 1

For the initial implementation, Phase 1 components should be built in this order:

1. **Value representation** - NaN-boxing macros and type checks
2. **Memory/heap** - Page allocator, object allocation
3. **Core types** - ObjString, ObjList, ObjMap, ObjBytes
4. **Lexer** - Token types and tokenization
5. **Parser** - AST nodes and recursive descent
6. **Resolver** - Scope analysis and capture detection
7. **Code generator** - Bytecode emission
8. **VM core** - Dispatch loop, opcodes, call frames
9. **GC** - Mark-and-sweep collection
10. **Built-in methods** - String/List/Map methods
11. **REPL** - Basic read-eval-print loop
12. **Test harness** - Internal tests for each component

---

## Risk Mitigation

| Risk | Mitigation |
|---|---|
| NaN-boxing edge cases (NaN, -0, inf) | Comprehensive test suite for value representation |
| GC correctness under closures | Stress tests with closure-heavy code |
| Fiber stack overflow | Guard pages, configurable stack size |
| Register allocation complexity | Start with simple linear allocation, optimize later |
| .vebc format changes | Version field in header, reject incompatible versions |
| Performance regression | Benchmark suite from Phase 1 onward |
