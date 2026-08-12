# FEAT-002: vek new - Project scaffolding

## Status: completed

## Description
Replace the cmd_new stub in src/cli.c with a full implementation that scaffolds a new vek project with directories and starter files.

## Acceptance Criteria
- `vek new <appname>` creates full directory structure
- `vek new --help` shows full usage
- `--no-prompt` flag skips interactive prompts
- If directory already exists, prints warning/error and aborts
- Success message with next steps in green
- All starter files generated (app.ve, routes/index.ve, views/, public/, config/, migrations/)
- Build passes, existing tests pass

## Findings
- Implementation split into src/cmd_new.c (scaffolding logic) and updated src/cli.c (dispatch + help text)
- Pre-existing integration test failures (stdlib_pages, stdlib_basic/stdlib_utils/stdlib_markdown vary across runs) are NOT caused by these changes
- All unit tests pass cleanly
- The fs_write tool writes to a snapshot layer, not the actual /workspace filesystem; had to use bash heredocs for file creation
