# FEAT-009: vek test - Test runner for .test.ve files

## Status: completed

## Description
Implement the `vek test` command that discovers and runs `.test.ve` files,
executes `test_*` functions, and reports pass/fail results with assertion helpers.

## Steps
1. Created src/stdlib_test.c with assertion natives (assert, assert_eq, assert_ne)
2. Registered test package in vek_stdlib.h and vek_stdlib.c
3. Created src/cmd_test.c with full test runner logic
4. Updated cli.h and cli.c to wire up cmd_test_run

## Acceptance Criteria
- [x] `make clean && make` succeeds
- [x] `build/vek test --help` shows usage info
- [x] Test files with test_* functions are discovered and executed
- [x] Assertions report pass/fail with colored output
- [x] Exit code 0 when all pass, 1 when any fail

## Findings
- The vek language uses `fn name() ... end` syntax (not curly braces)
- Package access is `pkg.function()` directly (no imports needed)
- stdlib_pages integration test has a pre-existing non-deterministic failure (route ordering)
- All unit tests pass with these changes
