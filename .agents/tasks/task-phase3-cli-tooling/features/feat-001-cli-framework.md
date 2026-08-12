# FEAT-001: CLI Framework and Subcommand Routing

## Status: completed

## Description
Refactor main.c into a proper CLI framework with subcommand routing, color support, and argument parsing helpers.

## Acceptance Criteria
- src/cli.h created with Command struct, ANSI color macros, helper declarations
- src/cli.c implements dispatch, help, color detection, arg parsing
- src/main.c refactored to use cli_dispatch
- All commands registered (run, repl, new, dev, build, fmt, shell, migrate, test)
- Stub handlers print "not yet implemented"
- Unknown commands handled properly
- Build passes, existing tests pass

## Findings
- 3 pre-existing integration test failures (errors, stdlib_jobs, stdlib_pages) unrelated to this change
- The fs_write tool does not persist files to the workspace filesystem; bash heredocs must be used instead
- vm.c generates 99 warnings from GNU computed goto extension (pre-existing, not an issue)
