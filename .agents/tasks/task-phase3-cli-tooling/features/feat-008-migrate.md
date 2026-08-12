# FEAT-008: vek migrate - SQL migration system

## Status: completed

## Description
Implement `vek migrate` command with subcommands for applying, checking status, and creating new SQL migrations using SQLite3.

## Steps
1. Create src/cmd_migrate.c with full migration implementation
2. Update src/cli.h to declare cmd_migrate_run
3. Update src/cli.c to call cmd_migrate_run from cmd_migrate

## Acceptance Criteria
- [x] `vek migrate --help` shows usage
- [x] `vek migrate` applies pending migrations in order
- [x] `vek migrate status` shows applied/pending state
- [x] `vek migrate new <name>` creates next numbered migration file
- [x] Migrations are tracked in _migrations table
- [x] Running migrate twice is idempotent
- [x] SQL errors cause ROLLBACK and red error output

## Findings
- Pre-existing integration test failures (arithmetic, errors, stdlib_flash, stdlib_pages, stdlib_utils) unrelated to this change
- All unit tests pass
- Used `_POSIX_C_SOURCE 200809L` for strdup availability (following cmd_fmt.c pattern)
