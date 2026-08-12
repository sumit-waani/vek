# FEAT-006: vek fmt - Opinionated code formatter for .ve files

## Status: completed

## Description
Implement the `vek fmt` command as an opinionated code formatter for .ve source files, using the existing lexer to tokenize and re-emit properly formatted code.

## Acceptance Criteria
- src/formatter.h and src/formatter.c implement the formatting engine
- src/cmd_fmt.c implements cmd_fmt_run with --check and --diff flags
- Recursive .ve file discovery with directory skipping (.git, build, node_modules)
- Proper indentation rules (2-space, block openers/closers)
- Space normalization around operators, commas, keywords
- No trailing whitespace, no multiple blank lines, single newline at EOF
- Comments preserved with proper indentation
- cli.c updated to call cmd_fmt_run
- cli.h updated with cmd_fmt_run declaration
- `make clean && make` succeeds
- Formatting is idempotent

## Findings
- The lexer skips comments entirely (in skip_whitespace), so the formatter uses a line-based approach that splits source into comment lines and code blocks, formats code blocks with the lexer, and preserves comments with proper indentation.
- Web keywords (TOKEN_GET, TOKEN_POST, etc.) are lexed as distinct tokens, not identifiers. The function-call no-space rule had to include them.
- Unary minus/plus is not distinguished from binary in the token stream; heuristic used based on preceding token type.
- test_view and test_db sometimes fail non-deterministically (pre-existing), not related to our changes.
