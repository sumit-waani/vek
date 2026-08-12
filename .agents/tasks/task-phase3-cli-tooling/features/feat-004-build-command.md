# FEAT-004: vek build - .vebc binary artifact packaging

## Status: completed

## Description
Implement the `vek build` command that compiles all .ve source files and packages them into a .vebc binary artifact per the bytecode specification.

## Acceptance Criteria
- [x] src/sha256.h and src/sha256.c expose sha256_compute for use by the writer
- [x] src/vebc_writer.h and src/vebc_writer.c implement the binary writer with VebcBuilder API
- [x] src/cmd_build.c implements cmd_build_run with --output flag, compile, and package
- [x] cli.h declares cmd_build_run, cli.c calls it
- [x] `vek build --help` shows usage with --output option
- [x] Build compiles app.ve and routes/*.ve, embeds public/ assets
- [x] Output .vebc starts with magic bytes 0x56 0x45 0x42 0x43
- [x] SHA-256 integrity hash computed over bytes 64..EOF
- [x] `make clean && make` succeeds
- [x] Build produces valid .vebc on project with valid .ve source

## Findings
- The scaffolded project template (from vek new) uses language features not yet
  supported by the compiler (// comments, import, dot notation). Building those
  projects fails at compilation, but the build command infrastructure works correctly
  with simpler .ve source files.
- Pre-existing test failures: stdlib_pages integration test fails on the base branch.
- All unit tests pass with these changes.
