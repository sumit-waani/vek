# vek Lexer / Tokenizer

## Overview

The lexer (tokenizer) is the first stage of the vek language pipeline. It reads raw source code as a string of characters and produces a stream of tokens that the parser consumes.

The lexer is implemented as a single-pass scanner that produces one token at a time on demand (no buffering of the entire token stream).

## Architecture

```
Source Code (const char*)
       |
       v
  +----------+
  |  Lexer   |  lexer_init() / lexer_next_token()
  +----------+
       |
       v
  Token stream (one at a time)
```

### Key Design Decisions

1. **On-demand scanning**: Tokens are produced one at a time via `lexer_next_token()`. No allocation is needed - tokens reference slices of the original source string.
2. **No heap allocation**: The Token struct stores a pointer (`start`) into the original source and a `length`. No strings are copied.
3. **Newlines are tokens**: `TOKEN_NEWLINE` is emitted for `\n` characters, allowing the parser to use newlines as statement terminators.
4. **Comments are skipped**: `#` to end-of-line comments are consumed during whitespace skipping.
5. **Identifier trailing `?`**: Identifiers can end with `?` (e.g., `active?`, `empty?`) UNLESS the `?` is followed by `.` or `:` (to preserve `?.` safe-nav and `?:` ternary operators).

## Token Types

### Single-Character Tokens

| Token | Character |
|-------|-----------|
| TOKEN_LPAREN | `(` |
| TOKEN_RPAREN | `)` |
| TOKEN_LBRACKET | `[` |
| TOKEN_RBRACKET | `]` |
| TOKEN_LBRACE | `{` |
| TOKEN_RBRACE | `}` |
| TOKEN_COMMA | `,` |
| TOKEN_DOT | `.` |
| TOKEN_SEMICOLON | `;` |
| TOKEN_COLON | `:` |
| TOKEN_PLUS | `+` |
| TOKEN_MINUS | `-` |
| TOKEN_STAR | `*` |
| TOKEN_SLASH | `/` |
| TOKEN_PERCENT | `%` |
| TOKEN_AMP | `&` |
| TOKEN_PIPE | `\|` |
| TOKEN_CARET | `^` |
| TOKEN_TILDE | `~` |
| TOKEN_LESS | `<` |
| TOKEN_GREATER | `>` |
| TOKEN_EQUAL | `=` |
| TOKEN_BANG | `!` |
| TOKEN_AT | `@` |
| TOKEN_QUESTION | `?` |

### Multi-Character Tokens

| Token | Characters |
|-------|-----------|
| TOKEN_EQUAL_EQUAL | `==` |
| TOKEN_BANG_EQUAL | `!=` |
| TOKEN_LESS_EQUAL | `<=` |
| TOKEN_GREATER_EQUAL | `>=` |
| TOKEN_AMP_AMP | `&&` |
| TOKEN_PIPE_PIPE | `\|\|` |
| TOKEN_DOT_DOT | `..` |
| TOKEN_DOT_DOT_DOT | `...` |
| TOKEN_ARROW | `->` |
| TOKEN_FAT_ARROW | `=>` |
| TOKEN_SAFE_NAV | `?.` |
| TOKEN_TERNARY | `?:` |
| TOKEN_LSHIFT | `<<` |
| TOKEN_RSHIFT | `>>` |
| TOKEN_PLUS_EQUAL | `+=` |
| TOKEN_MINUS_EQUAL | `-=` |
| TOKEN_STAR_EQUAL | `*=` |
| TOKEN_SLASH_EQUAL | `/=` |
| TOKEN_STAR_STAR | `**` |

### Literal Tokens

- `TOKEN_INT` - Integer literals (e.g., `42`, `1_000_000`)
- `TOKEN_FLOAT` - Float literals (e.g., `3.14`, `1e9`, `1.5e-3`)
- `TOKEN_STRING` - String literals (single or double quotes)
- `TOKEN_SYMBOL` - Symbol literals (`:foo`, `:"with spaces"`)
- `TOKEN_IDENTIFIER` - Identifiers (variable/function names)

### Keywords

All reserved words from the language spec:

```
fn end do if elsif else then while until loop for in case
return break next true false nil and or not begin rescue raise
unless module get post put patch delete render redirect halt
```

### Special Tokens

- `TOKEN_EOF` - End of input
- `TOKEN_ERROR` - Malformed input (unterminated strings, invalid characters)
- `TOKEN_NEWLINE` - Newline character (statement separator)
- `TOKEN_INTERPOLATION_START` - `#{` inside a double-quoted string

## Usage

```c
#include "lexer.h"

Lexer lexer;
lexer_init(&lexer, "fn add(a, b)\n  a + b\nend");

Token token;
do {
    token = lexer_next_token(&lexer);
    printf("Token: type=%d line=%d [%.*s]\n",
           token.type, token.line, token.length, token.start);
} while (token.type != TOKEN_EOF);
```

## Edge Cases

### Range Operators vs Method Calls

- `1..10` produces: INT, DOT_DOT, INT
- `1...10` produces: INT, DOT_DOT_DOT, INT
- `obj.method` produces: IDENTIFIER, DOT, IDENTIFIER

### Safe Navigation

- `obj?.method` produces: IDENTIFIER("obj"), SAFE_NAV, IDENTIFIER("method")
- The `?` is NOT consumed as part of the identifier when followed by `.`

### Arrows

- `a -> b` produces: IDENTIFIER, ARROW, IDENTIFIER
- `a - > b` produces: IDENTIFIER, MINUS, GREATER, IDENTIFIER

### Integer Separators

- `1_000_000` is a single TOKEN_INT (underscores are included in the token text)
- The parser/compiler is responsible for stripping underscores when converting to a numeric value

### String Interpolation

- When the lexer encounters `#{` inside a double-quoted string, it emits `TOKEN_INTERPOLATION_START`
- The parser is responsible for tracking string interpolation context
- Single-quoted strings do NOT support interpolation

## File Layout

- `src/lexer.h` - Public API (TokenType enum, Token struct, Lexer struct, function declarations)
- `src/lexer.c` - Implementation
- `tests/test_lexer.c` - Unit tests (31 tests covering all token types and edge cases)
