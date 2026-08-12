# FEAT-007: vek repl / vek shell - Enhanced REPL with multi-line input and shell mode

## Status: completed

## Description
Implement enhanced REPL with multi-line input detection and shell mode that loads app context before entering REPL.

## Acceptance Criteria
- [x] src/cmd_repl.c implements cmd_repl_run with colored prompts, multi-line input, history, exit/Ctrl-D
- [x] src/cmd_shell.c implements cmd_shell_run with app.ve loading and shell mode
- [x] Multi-line detection via lexer token scanning (block openers/closers, parens, brackets, braces)
- [x] Result printing for expression evaluation (integers, floats, strings, lists, maps, nil, booleans)
- [x] cli.h updated with cmd_repl_run and cmd_shell_run declarations
- [x] cli.c updated to call the new functions
- [x] `make clean && make` succeeds
- [x] `echo 'exit' | build/vek repl` exits cleanly
- [x] Multi-line function definition works (continuation prompts shown)
- [x] `build/vek shell --help` shows usage
- [x] `build/vek shell` without app.ve prints error

## Findings
- vm_interpret does not leave expression results on stack for top-level script execution (the script's implicit return pops values). Result printing only fires if the VM happens to leave a value above the pre-execution stack pointer.
- Pre-existing test failure: stdlib_pages integration test fails due to whitespace/line-ending mismatch (not caused by this change).
- strdup requires `#define _POSIX_C_SOURCE 200809L` at the top of .c files (consistent with cmd_fmt.c pattern).
