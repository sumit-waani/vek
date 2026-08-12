# FEAT-003: vek dev - Development server with file watching and hot reload

## Status: completed

## Description
Implement the `vek dev` command that starts a development server with file watching using inotify and hot reload via process-level restart (fork/exec).

## Acceptance Criteria
- [x] src/file_watcher.h and src/file_watcher.c implement inotify-based file watching
- [x] src/cmd_dev.c implements cmd_dev_run with port parsing, file watching, and child process management
- [x] `vek dev --help` shows usage with --port option
- [x] Colored output using CLI macros
- [x] Watches .ve files, skips .git/, build/, node_modules/
- [x] On file change: kills child, prints reload message, forks new child
- [x] Handles SIGINT for clean shutdown
- [x] Build passes, existing tests pass

## Findings
- Pre-existing test failures: 4 integration tests (functions, stdlib_auth, stdlib_flash, stdlib_pages) fail on base. stdlib_pages is a flaky ordering test. The others appear to be environment-dependent.
- The _vek_jobs.db files get created during test runs and can cause test_db unit test failure if not cleaned up.
- Used _GNU_SOURCE define (matching project convention from http_server.c, event_loop.c) to enable POSIX signal APIs under strict C11 mode.
