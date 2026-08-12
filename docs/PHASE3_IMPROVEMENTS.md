# Phase 3 CLI Tooling - Non-blocking Improvement Issues

These issues were identified during the semantic review of Phase 3 (CLI Tooling).
They are non-blocking and do not prevent the current implementation from functioning,
but should be addressed to improve robustness and user experience.

---

## 1. VebcBuilder Stack Overflow Risk

**Location:** `cmd_build_run` / `VebcBuilder` struct

**Problem:**
The `VebcBuilder` struct is approximately 230 KB due to fixed-size arrays (4096 strings, 4096 constants, 1024 functions, 4096 upvalues, 1024 assets). It is currently allocated on the stack. This risks a stack overflow on platforms with limited stack sizes or in deeply nested call paths.

**Suggested Fix:**
Move `VebcBuilder` to heap allocation (via `malloc`/`calloc`) or significantly reduce the fixed array limits. If heap-allocated, ensure proper cleanup on all exit paths.

---

## 2. Worker Restart Tight-Loop

**Location:** `run_with_workers` in `cli.c`

**Problem:**
When a worker exits non-zero or is killed by a signal, it is immediately restarted with no backoff delay or maximum restart count. A binary that segfaults on startup will cause an endless fork loop, consuming system resources.

**Suggested Fix:**
- Track the timestamp of the last restart per worker.
- Refuse to restart if the worker died within 1 second of its last start.
- Cap total restarts at N (e.g., 5) before giving up and reporting an error.

---

## 3. Nested Function Stubs in .vebc Loader

**Location:** `.vebc` file loader, `CONST_TAG_FUNC_REF` handling

**Problem:**
`CONST_TAG_FUNC_REF` constants are loaded as `VAL_NIL`. Any compiled source that defines closures or nested functions will silently produce broken bytecode when loaded from a `.vebc` file. This leads to runtime failures with no indication of what went wrong.

**Suggested Fix:**
- At minimum, emit a warning during load when a `CONST_TAG_FUNC_REF` is encountered.
- Preferably, return an error from `vebc_to_function` when unresolved function references are detected, so the user gets a clear diagnostic.

---

## 4. Dev Server Lacks Debounce

**Location:** inotify-based file watching in the dev server

**Problem:**
Each inotify event triggers a kill/restart cycle with no coalescing window. Editors like vim and VSCode fire multiple events per save operation, causing redundant and rapid restarts.

**Suggested Fix:**
Implement a 100-200ms coalesce window: after receiving the first event, drain all pending events during the window, then trigger a single restart. This prevents multiple redundant restarts per save.

---

## 5. Formatter Inline Comment Loss

**Location:** `vek fmt` command, comment/code splitting logic

**Problem:**
The formatter splits source into comment lines and code blocks before tokenizing. Inline comments (code followed by `# comment` on the same line) are silently stripped. Users who use inline comments will lose them when running `vek fmt` with no warning.

**Suggested Fix:**
- Detect inline comments during the split phase and either preserve them or emit a warning that they will be removed.
- Ideally, the tokenizer should handle inline comments as tokens associated with the preceding code, allowing the formatter to re-emit them in the output.
