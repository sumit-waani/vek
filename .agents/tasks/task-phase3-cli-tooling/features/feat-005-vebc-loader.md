# FEAT-005: vek run - Load and verify .vebc artifacts

## Status: completed

## Description
Implement the .vebc loader that can read compiled bytecode artifacts and execute them via the VM, along with --workers and --port flags for the run command.

## Acceptance Criteria
- src/vebc_loader.h and src/vebc_loader.c implement the binary reader with VebcFile struct
- vebc_load() mmaps the file, parses header, validates magic/version
- vebc_verify() computes SHA-256 of bytes 64..EOF and compares with header hash
- vebc_to_function() reconstructs ObjFunction from bytecode for VM execution
- vebc_free() unmaps and frees resources
- cmd_run in cli.c detects .vebc files and uses the loader path
- cmd_run supports --workers=N flag for multi-process execution
- cmd_run supports --port=N flag to set PORT env var
- `make clean && make` succeeds
- `build/vek run --help` shows updated usage with --workers and --port options

## Findings
- Pre-existing integration test failures (stdlib_pages, errors) are non-deterministic and unrelated to this feature
- gc_push_root takes a Value (not ObjHeader*) - use OBJ_VAL() macro to wrap
- vm_call can be used to execute loaded ObjFunction wrapped in ObjClosure
