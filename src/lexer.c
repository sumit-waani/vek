/*
 * vek lexer - tokenizer for the vek language.
 *
 * Handles: keywords, identifiers, integer/float literals, string literals
 * (with interpolation detection), symbol literals, operators, and punctuation.
 */

#include "lexer.h"
#include <ctype.h>

// ---- Helper functions ----

static bool is_at_end(Lexer* lexer) {
    return *lexer->current == '\0';
}

static char advance(Lexer* lexer) {
    lexer->current++;
    return lexer->current[-1];
}

static char peek(Lexer* lexer) {
    return *lexer->current;
}

static char peek_next(Lexer* lexer) {
    if (is_at_end(lexer)) return '\0';
    return lexer->current[1];
}

static bool match(Lexer* lexer, char expected) {
    if (is_at_end(lexer)) return false;
    if (*lexer->current != expected) return false;
    lexer->current++;
    return true;
}

static Token make_token(Lexer* lexer, TokenType type) {
    Token token;
    token.type = type;
    token.start = lexer->start;
    token.length = (int)(lexer->current - lexer->start);
    token.line = lexer->line;
    return token;
}

static Token error_token(Lexer* lexer, const char* message) {
    Token token;
    token.type = TOKEN_ERROR;
    token.start = message;
    token.length = (int)strlen(message);
    token.line = lexer->line;
    return token;
}

// ---- Whitespace and comments ----

static void skip_whitespace(Lexer* lexer) {
    for (;;) {
        char c = peek(lexer);
        switch (c) {
            case ' ':
            case '\t':
            case '\r':
                advance(lexer);
                break;
            case '#':
                // Comment: skip to end of line
                while (peek(lexer) != '\n' && !is_at_end(lexer)) {
                    advance(lexer);
                }
                break;
            default:
                return;
        }
    }
}

// ---- Keyword identification ----

typedef struct {
    const char* name;
    int length;
    TokenType type;
} Keyword;

static const Keyword keywords[] = {
    {"and",      3, TOKEN_AND},
    {"begin",    5, TOKEN_BEGIN},
    {"break",    5, TOKEN_BREAK},
    {"case",     4, TOKEN_CASE},
    {"delete",   6, TOKEN_DELETE},
    {"do",       2, TOKEN_DO},
    {"else",     4, TOKEN_ELSE},
    {"elsif",    5, TOKEN_ELSIF},
    {"end",      3, TOKEN_END},
    {"false",    5, TOKEN_FALSE},
    {"fn",       2, TOKEN_FN},
    {"for",      3, TOKEN_FOR},
    {"get",      3, TOKEN_GET},
    {"halt",     4, TOKEN_HALT},
    {"if",       2, TOKEN_IF},
    {"in",       2, TOKEN_IN},
    {"loop",     4, TOKEN_LOOP},
    {"module",   6, TOKEN_MODULE},
    {"next",     4, TOKEN_NEXT},
    {"nil",      3, TOKEN_NIL},
    {"not",      3, TOKEN_NOT},
    {"or",       2, TOKEN_OR},
    {"patch",    5, TOKEN_PATCH},
    {"post",     4, TOKEN_POST},
    {"put",      3, TOKEN_PUT},
    {"raise",    5, TOKEN_RAISE},
    {"redirect", 8, TOKEN_REDIRECT},
    {"render",   6, TOKEN_RENDER},
    {"rescue",   6, TOKEN_RESCUE},
    {"return",   6, TOKEN_RETURN},
    {"then",     4, TOKEN_THEN},
    {"true",     4, TOKEN_TRUE},
    {"unless",   6, TOKEN_UNLESS},
    {"until",    5, TOKEN_UNTIL},
    {"while",    5, TOKEN_WHILE},
    {NULL,       0, TOKEN_IDENTIFIER}  // sentinel
};

static TokenType identifier_type(const char* start, int length) {
    for (int i = 0; keywords[i].name != NULL; i++) {
        if (keywords[i].length == length &&
            memcmp(start, keywords[i].name, (size_t)length) == 0) {
            return keywords[i].type;
        }
    }
    return TOKEN_IDENTIFIER;
}

// ---- Scanning helpers ----

static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '_';
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static Token scan_identifier(Lexer* lexer) {
    while (is_alpha(peek(lexer)) || is_digit(peek(lexer))) {
        advance(lexer);
    }
    // Allow trailing ? for predicate-style identifiers like active?, empty?
    // But NOT if followed by . or : (which would be ?. safe-nav or ?: ternary)
    if (peek(lexer) == '?' && peek_next(lexer) != '.' && peek_next(lexer) != ':') {
        advance(lexer);
    }

    int length = (int)(lexer->current - lexer->start);
    TokenType type = identifier_type(lexer->start, length);
    return make_token(lexer, type);
}

static Token scan_number(Lexer* lexer) {
    // Scan integer part (allowing underscore separators)
    while (is_digit(peek(lexer)) || peek(lexer) == '_') {
        advance(lexer);
    }

    bool is_float = false;

    // Check for decimal point (but not range operator ..)
    if (peek(lexer) == '.' && peek_next(lexer) != '.' &&
        is_digit(peek_next(lexer))) {
        is_float = true;
        advance(lexer); // consume '.'
        while (is_digit(peek(lexer)) || peek(lexer) == '_') {
            advance(lexer);
        }
    }

    // Check for exponent
    if (peek(lexer) == 'e' || peek(lexer) == 'E') {
        is_float = true;
        advance(lexer); // consume 'e'/'E'
        if (peek(lexer) == '+' || peek(lexer) == '-') {
            advance(lexer); // consume sign
        }
        if (!is_digit(peek(lexer))) {
            return error_token(lexer, "Invalid number: expected digit after exponent");
        }
        while (is_digit(peek(lexer))) {
            advance(lexer);
        }
    }

    return make_token(lexer, is_float ? TOKEN_FLOAT : TOKEN_INT);
}

static Token scan_string(Lexer* lexer, char quote) {
    while (!is_at_end(lexer)) {
        char c = peek(lexer);

        if (c == '\n') {
            lexer->line++;
        }

        if (c == quote) {
            advance(lexer); // consume closing quote
            return make_token(lexer, TOKEN_STRING);
        }

        if (c == '\\') {
            advance(lexer); // consume backslash
            // Skip the escaped character
            if (!is_at_end(lexer)) {
                if (peek(lexer) == '\n') lexer->line++;
                advance(lexer);
            }
            continue;
        }

        // String interpolation detection for double-quoted strings
        if (quote == '"' && c == '#' && peek_next(lexer) == '{') {
            // Emit the string portion up to here (including opening quote)
            // If start == current - 1 (just the opening quote), still emit it
            // Return string token for the part scanned so far
            Token token = make_token(lexer, TOKEN_STRING);
            // Now advance past #{ so next call starts after it
            lexer->current += 2; // skip #{
            // Emit interpolation start instead
            // Actually: return the string part, then handle #{ on next call.
            // But we need to emit INTERPOLATION_START somewhere.
            // Simplest: just emit INTERPOLATION_START here, caller handles context.
            token.type = TOKEN_INTERPOLATION_START;
            token.start = lexer->current - 2; // point at #
            token.length = 2;
            return token;
        }

        advance(lexer);
    }

    return error_token(lexer, "Unterminated string");
}

static Token scan_symbol(Lexer* lexer) {
    // We already consumed the ':'
    if (peek(lexer) == '"') {
        // Quoted symbol: :"with spaces"
        advance(lexer); // consume opening "
        while (!is_at_end(lexer) && peek(lexer) != '"') {
            if (peek(lexer) == '\\') {
                advance(lexer);
                if (!is_at_end(lexer)) advance(lexer);
                continue;
            }
            if (peek(lexer) == '\n') lexer->line++;
            advance(lexer);
        }
        if (is_at_end(lexer)) {
            return error_token(lexer, "Unterminated symbol string");
        }
        advance(lexer); // consume closing "
        return make_token(lexer, TOKEN_SYMBOL);
    }

    // Bare symbol: :identifier
    if (is_alpha(peek(lexer))) {
        while (is_alpha(peek(lexer)) || is_digit(peek(lexer))) {
            advance(lexer);
        }
        return make_token(lexer, TOKEN_SYMBOL);
    }

    // Just a colon by itself (not a symbol)
    return make_token(lexer, TOKEN_COLON);
}

// ---- Public API ----

void lexer_init(Lexer* lexer, const char* source) {
    lexer->start = source;
    lexer->current = source;
    lexer->line = 1;
}

Token lexer_next_token(Lexer* lexer) {
    skip_whitespace(lexer);
    lexer->start = lexer->current;

    if (is_at_end(lexer)) {
        return make_token(lexer, TOKEN_EOF);
    }

    char c = advance(lexer);

    // Identifiers and keywords
    if (is_alpha(c)) {
        return scan_identifier(lexer);
    }

    // Numbers
    if (is_digit(c)) {
        return scan_number(lexer);
    }

    switch (c) {
        // Newline
        case '\n': {
            Token t = make_token(lexer, TOKEN_NEWLINE);
            lexer->line++;
            return t;
        }

        // Single-character tokens
        case '(': return make_token(lexer, TOKEN_LPAREN);
        case ')': return make_token(lexer, TOKEN_RPAREN);
        case '[': return make_token(lexer, TOKEN_LBRACKET);
        case ']': return make_token(lexer, TOKEN_RBRACKET);
        case '{': return make_token(lexer, TOKEN_LBRACE);
        case '}': return make_token(lexer, TOKEN_RBRACE);
        case ',': return make_token(lexer, TOKEN_COMMA);
        case ';': return make_token(lexer, TOKEN_SEMICOLON);
        case '~': return make_token(lexer, TOKEN_TILDE);
        case '@': return make_token(lexer, TOKEN_AT);
        case '%': return make_token(lexer, TOKEN_PERCENT);
        case '^': return make_token(lexer, TOKEN_CARET);

        // Colon / Symbol
        case ':':
            return scan_symbol(lexer);

        // Dot / Range
        case '.':
            if (match(lexer, '.')) {
                if (match(lexer, '.')) {
                    return make_token(lexer, TOKEN_DOT_DOT_DOT);
                }
                return make_token(lexer, TOKEN_DOT_DOT);
            }
            return make_token(lexer, TOKEN_DOT);

        // Plus
        case '+':
            if (match(lexer, '=')) return make_token(lexer, TOKEN_PLUS_EQUAL);
            return make_token(lexer, TOKEN_PLUS);

        // Minus / Arrow
        case '-':
            if (match(lexer, '>')) return make_token(lexer, TOKEN_ARROW);
            if (match(lexer, '=')) return make_token(lexer, TOKEN_MINUS_EQUAL);
            return make_token(lexer, TOKEN_MINUS);

        // Star / Power
        case '*':
            if (match(lexer, '*')) return make_token(lexer, TOKEN_STAR_STAR);
            if (match(lexer, '=')) return make_token(lexer, TOKEN_STAR_EQUAL);
            return make_token(lexer, TOKEN_STAR);

        // Slash
        case '/':
            if (match(lexer, '=')) return make_token(lexer, TOKEN_SLASH_EQUAL);
            return make_token(lexer, TOKEN_SLASH);

        // Less / Shift / Arrow
        case '<':
            if (match(lexer, '<')) return make_token(lexer, TOKEN_LSHIFT);
            if (match(lexer, '=')) return make_token(lexer, TOKEN_LESS_EQUAL);
            return make_token(lexer, TOKEN_LESS);

        // Greater / Shift
        case '>':
            if (match(lexer, '>')) return make_token(lexer, TOKEN_RSHIFT);
            if (match(lexer, '=')) return make_token(lexer, TOKEN_GREATER_EQUAL);
            return make_token(lexer, TOKEN_GREATER);

        // Equal / Fat arrow
        case '=':
            if (match(lexer, '=')) return make_token(lexer, TOKEN_EQUAL_EQUAL);
            if (match(lexer, '>')) return make_token(lexer, TOKEN_FAT_ARROW);
            return make_token(lexer, TOKEN_EQUAL);

        // Bang
        case '!':
            if (match(lexer, '=')) return make_token(lexer, TOKEN_BANG_EQUAL);
            return make_token(lexer, TOKEN_BANG);

        // Ampersand
        case '&':
            if (match(lexer, '&')) return make_token(lexer, TOKEN_AMP_AMP);
            return make_token(lexer, TOKEN_AMP);

        // Pipe
        case '|':
            if (match(lexer, '|')) return make_token(lexer, TOKEN_PIPE_PIPE);
            return make_token(lexer, TOKEN_PIPE);

        // Question mark / Safe nav / Ternary
        case '?':
            if (match(lexer, '.')) return make_token(lexer, TOKEN_SAFE_NAV);
            if (match(lexer, ':')) return make_token(lexer, TOKEN_TERNARY);
            return make_token(lexer, TOKEN_QUESTION);

        // String literals
        case '"': return scan_string(lexer, '"');
        case '\'': return scan_string(lexer, '\'');
    }

    return error_token(lexer, "Unexpected character");
}
