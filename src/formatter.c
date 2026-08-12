#include "common.h"
#include "formatter.h"
#include "lexer.h"

#include <ctype.h>

// ---- Dynamic buffer for output ----

typedef struct {
    char* data;
    size_t length;
    size_t capacity;
} FmtBuffer;

static void buf_init(FmtBuffer* buf) {
    buf->data = NULL;
    buf->length = 0;
    buf->capacity = 0;
}

static void buf_ensure(FmtBuffer* buf, size_t additional) {
    if (buf->length + additional >= buf->capacity) {
        size_t new_cap = buf->capacity < 256 ? 256 : buf->capacity * 2;
        while (new_cap < buf->length + additional + 1) {
            new_cap *= 2;
        }
        buf->data = (char*)realloc(buf->data, new_cap);
        buf->capacity = new_cap;
    }
}

static void buf_append(FmtBuffer* buf, const char* str, size_t len) {
    buf_ensure(buf, len);
    memcpy(buf->data + buf->length, str, len);
    buf->length += len;
}

static void buf_append_char(FmtBuffer* buf, char c) {
    buf_ensure(buf, 1);
    buf->data[buf->length++] = c;
}

// ---- Token classification helpers ----

static bool is_block_opener(TokenType type) {
    switch (type) {
        case TOKEN_FN:
        case TOKEN_IF:
        case TOKEN_WHILE:
        case TOKEN_UNTIL:
        case TOKEN_LOOP:
        case TOKEN_FOR:
        case TOKEN_DO:
        case TOKEN_CASE:
        case TOKEN_BEGIN:
        case TOKEN_UNLESS:
        case TOKEN_MODULE:
            return true;
        default:
            return false;
    }
}

static bool is_block_closer(TokenType type) {
    return type == TOKEN_END;
}

static bool is_continuation(TokenType type) {
    switch (type) {
        case TOKEN_ELSIF:
        case TOKEN_ELSE:
        case TOKEN_RESCUE:
            return true;
        default:
            return false;
    }
}

static bool is_binary_operator(TokenType type) {
    switch (type) {
        case TOKEN_PLUS:
        case TOKEN_MINUS:
        case TOKEN_STAR:
        case TOKEN_SLASH:
        case TOKEN_PERCENT:
        case TOKEN_EQUAL:
        case TOKEN_EQUAL_EQUAL:
        case TOKEN_BANG_EQUAL:
        case TOKEN_LESS:
        case TOKEN_GREATER:
        case TOKEN_LESS_EQUAL:
        case TOKEN_GREATER_EQUAL:
        case TOKEN_AMP_AMP:
        case TOKEN_PIPE_PIPE:
        case TOKEN_PLUS_EQUAL:
        case TOKEN_MINUS_EQUAL:
        case TOKEN_STAR_EQUAL:
        case TOKEN_SLASH_EQUAL:
        case TOKEN_STAR_STAR:
        case TOKEN_PIPE:
        case TOKEN_AMP:
        case TOKEN_CARET:
        case TOKEN_LSHIFT:
        case TOKEN_RSHIFT:
        case TOKEN_AND:
        case TOKEN_OR:
        case TOKEN_DOT_DOT:
            return true;
        default:
            return false;
    }
}

static bool is_keyword_with_expr(TokenType type) {
    switch (type) {
        case TOKEN_IF:
        case TOKEN_ELSIF:
        case TOKEN_WHILE:
        case TOKEN_UNTIL:
        case TOKEN_FOR:
        case TOKEN_UNLESS:
        case TOKEN_RETURN:
        case TOKEN_RAISE:
        case TOKEN_CASE:
            return true;
        default:
            return false;
    }
}

// ---- Line-based formatting with comment preservation ----

// A source line descriptor
typedef struct {
    const char* start;
    size_t length;
    bool is_comment;    // line is purely a comment (starts with # after whitespace)
    bool is_blank;      // line is entirely whitespace
} SourceLine;

// Split source into lines
static SourceLine* split_lines(const char* source, size_t* out_count) {
    size_t capacity = 64;
    size_t count = 0;
    SourceLine* lines = (SourceLine*)malloc(sizeof(SourceLine) * capacity);

    const char* p = source;
    while (*p) {
        const char* line_start = p;
        while (*p && *p != '\n') p++;
        size_t line_len = (size_t)(p - line_start);
        if (*p == '\n') p++;

        if (count >= capacity) {
            capacity *= 2;
            lines = (SourceLine*)realloc(lines, sizeof(SourceLine) * capacity);
        }

        lines[count].start = line_start;
        lines[count].length = line_len;

        // Check if blank
        bool blank = true;
        bool comment = false;
        for (size_t i = 0; i < line_len; i++) {
            if (line_start[i] != ' ' && line_start[i] != '\t' && line_start[i] != '\r') {
                blank = false;
                if (line_start[i] == '#') {
                    comment = true;
                }
                break;
            }
        }
        lines[count].is_blank = blank;
        lines[count].is_comment = comment;
        count++;
    }

    *out_count = count;
    return lines;
}

// Get comment content trimmed of leading whitespace
static const char* get_comment_text(SourceLine* line, size_t* out_len) {
    const char* p = line->start;
    const char* end = line->start + line->length;
    // Skip leading whitespace
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    *out_len = (size_t)(end - p);
    // Remove trailing whitespace
    while (*out_len > 0 && (p[*out_len - 1] == ' ' || p[*out_len - 1] == '\t' || p[*out_len - 1] == '\r')) {
        (*out_len)--;
    }
    return p;
}

// ---- Token-based code formatter ----

typedef struct {
    FmtBuffer out;
    int indent_level;
    int col;
    bool line_started;
} CodeFmtState;

static void code_emit_indent(CodeFmtState* state) {
    if (!state->line_started) {
        for (int i = 0; i < state->indent_level * 2; i++) {
            buf_append_char(&state->out, ' ');
        }
        state->col = state->indent_level * 2;
        state->line_started = true;
    }
}

static void code_emit_newline(CodeFmtState* state) {
    // Remove trailing whitespace
    while (state->out.length > 0 && state->out.data[state->out.length - 1] == ' ') {
        state->out.length--;
    }
    buf_append_char(&state->out, '\n');
    state->col = 0;
    state->line_started = false;
}

static void code_emit_token(CodeFmtState* state, Token* tok) {
    code_emit_indent(state);
    buf_append(&state->out, tok->start, (size_t)tok->length);
    state->col += tok->length;
}

static void code_emit_space(CodeFmtState* state) {
    if (state->line_started && state->out.length > 0 &&
        state->out.data[state->out.length - 1] != ' ' &&
        state->out.data[state->out.length - 1] != '\n') {
        buf_append_char(&state->out, ' ');
        state->col++;
    }
}

// Format a block of code lines (non-comment). Returns formatted string (caller frees).
// indent_base is the starting indent level for this block.
static char* format_code_block(const char* code, int* indent_level) {
    Lexer lexer;
    lexer_init(&lexer, code);

    // Collect tokens
    size_t tok_count = 0;
    size_t tok_cap = 128;
    Token* tokens = (Token*)malloc(sizeof(Token) * tok_cap);

    for (;;) {
        Token tok = lexer_next_token(&lexer);
        if (tok_count >= tok_cap) {
            tok_cap *= 2;
            tokens = (Token*)realloc(tokens, sizeof(Token) * tok_cap);
        }
        tokens[tok_count++] = tok;
        if (tok.type == TOKEN_EOF) break;
    }

    CodeFmtState state;
    memset(&state, 0, sizeof(state));
    buf_init(&state.out);
    state.indent_level = *indent_level;

    bool prev_was_newline = true;
    int blank_lines = 0;
    int fn_depth = 0;
    bool had_fn_block = false;

    for (size_t i = 0; i < tok_count; i++) {
        Token* tok = &tokens[i];

        if (tok->type == TOKEN_EOF) break;

        if (tok->type == TOKEN_ERROR) {
            code_emit_indent(&state);
            buf_append(&state.out, tok->start, (size_t)tok->length);
            state.col += tok->length;
            continue;
        }

        if (tok->type == TOKEN_NEWLINE) {
            if (prev_was_newline) {
                blank_lines++;
            } else {
                blank_lines = 0;
            }
            if (blank_lines <= 1) {
                code_emit_newline(&state);
            }
            prev_was_newline = true;
            continue;
        }

        // Indent adjustments
        if (is_block_closer(tok->type)) {
            if (state.indent_level > 0) state.indent_level--;
            if (fn_depth > 0) {
                fn_depth--;
                if (fn_depth == 0) had_fn_block = true;
            }
        }

        if (is_continuation(tok->type)) {
            if (state.indent_level > 0) state.indent_level--;
        }

        // Blank line between fn...end blocks
        if (tok->type == TOKEN_FN && fn_depth == 0 && had_fn_block && prev_was_newline) {
            if (blank_lines == 0 && state.out.length > 0) {
                code_emit_newline(&state);
            }
        }

        blank_lines = 0;

        // Spacing
        if (!prev_was_newline) {
            if (tok->type == TOKEN_COMMA || tok->type == TOKEN_SEMICOLON) {
                // no space before
            } else if (tok->type == TOKEN_RPAREN || tok->type == TOKEN_RBRACKET || tok->type == TOKEN_RBRACE) {
                // no space before ) ] }
            } else if (tok->type == TOKEN_DOT) {
                // no space before .
            } else if (tok->type == TOKEN_COLON) {
                // no space before : (map key separator)
            } else if (i > 0) {
                Token* prev = &tokens[i - 1];

                if (prev->type == TOKEN_LPAREN || prev->type == TOKEN_LBRACKET || prev->type == TOKEN_LBRACE) {
                    // no space after ( [ {
                } else if (prev->type == TOKEN_DOT) {
                    // no space after .
                } else if ((tok->type == TOKEN_LPAREN || tok->type == TOKEN_LBRACKET) &&
                           (prev->type == TOKEN_IDENTIFIER ||
                            prev->type == TOKEN_RPAREN ||
                            prev->type == TOKEN_RBRACKET ||
                            prev->type == TOKEN_GET ||
                            prev->type == TOKEN_POST ||
                            prev->type == TOKEN_PUT ||
                            prev->type == TOKEN_PATCH ||
                            prev->type == TOKEN_DELETE ||
                            prev->type == TOKEN_RENDER ||
                            prev->type == TOKEN_REDIRECT ||
                            prev->type == TOKEN_HALT ||
                            prev->type == TOKEN_STRING)) {
                    // no space before ( or [ in calls/subscripts
                } else if (is_binary_operator(tok->type)) {
                    // Unary minus/plus after ( or [
                    if ((tok->type == TOKEN_MINUS || tok->type == TOKEN_PLUS) &&
                        (prev->type == TOKEN_LPAREN ||
                         prev->type == TOKEN_LBRACKET)) {
                        // unary, no space
                    } else {
                        code_emit_space(&state);
                    }
                } else if (is_binary_operator(prev->type)) {
                    // If prev is unary minus/plus (after ( [ , or binary op)
                    if ((prev->type == TOKEN_MINUS || prev->type == TOKEN_PLUS) && i >= 2) {
                        Token* prevprev = &tokens[i - 2];
                        if (prevprev->type == TOKEN_LPAREN ||
                            prevprev->type == TOKEN_LBRACKET ||
                            prevprev->type == TOKEN_COMMA ||
                            is_binary_operator(prevprev->type)) {
                            // unary, no space after
                        } else {
                            code_emit_space(&state);
                        }
                    } else {
                        code_emit_space(&state);
                    }
                } else if (prev->type == TOKEN_COMMA) {
                    code_emit_space(&state);
                } else if (is_keyword_with_expr(prev->type)) {
                    code_emit_space(&state);
                } else if (prev->type == TOKEN_FN) {
                    code_emit_space(&state);
                } else if (prev->type == TOKEN_COLON) {
                    code_emit_space(&state);
                } else if (prev->type == TOKEN_DO) {
                    code_emit_space(&state);
                } else if (prev->type == TOKEN_IN) {
                    code_emit_space(&state);
                } else if (tok->type == TOKEN_LBRACE) {
                    code_emit_space(&state);
                } else {
                    code_emit_space(&state);
                }
            }
        }

        code_emit_token(&state, tok);
        prev_was_newline = false;

        // Post-emit indent changes
        if (is_block_opener(tok->type)) {
            state.indent_level++;
            if (tok->type == TOKEN_FN) fn_depth++;
        }
        if (is_continuation(tok->type)) {
            state.indent_level++;
        }
    }

    *indent_level = state.indent_level;
    free(tokens);

    // Null-terminate
    buf_append_char(&state.out, '\0');
    state.out.length--;

    return state.out.data;
}

// ---- Main formatting logic ----

char* fmt_format(const char* source, size_t length, size_t* out_length) {
    (void)length;

    // Split into lines
    size_t line_count = 0;
    SourceLine* lines = split_lines(source, &line_count);

    FmtBuffer result;
    buf_init(&result);

    int indent_level = 0;
    int consecutive_blanks = 0;

    size_t i = 0;
    while (i < line_count) {
        SourceLine* line = &lines[i];

        if (line->is_blank) {
            consecutive_blanks++;
            if (consecutive_blanks <= 1) {
                buf_append_char(&result, '\n');
            }
            i++;
            continue;
        }

        consecutive_blanks = 0;

        if (line->is_comment) {
            // Emit comment with current indentation
            size_t comment_len = 0;
            const char* comment_text = get_comment_text(line, &comment_len);
            // Apply indent
            for (int j = 0; j < indent_level * 2; j++) {
                buf_append_char(&result, ' ');
            }
            buf_append(&result, comment_text, comment_len);
            buf_append_char(&result, '\n');
            i++;
            continue;
        }

        // Collect consecutive code lines (non-comment, non-blank)
        size_t code_start = i;
        while (i < line_count && !lines[i].is_comment && !lines[i].is_blank) {
            i++;
        }

        // Build a code string from these lines
        FmtBuffer code_buf;
        buf_init(&code_buf);
        for (size_t j = code_start; j < i; j++) {
            buf_append(&code_buf, lines[j].start, lines[j].length);
            buf_append_char(&code_buf, '\n');
        }
        buf_append_char(&code_buf, '\0');

        // Format the code block
        char* formatted = format_code_block(code_buf.data, &indent_level);
        if (formatted) {
            // Append formatted code (it may have trailing newline already)
            size_t flen = strlen(formatted);
            buf_append(&result, formatted, flen);
            // Ensure it ends with newline
            if (flen > 0 && formatted[flen - 1] != '\n') {
                buf_append_char(&result, '\n');
            }
            free(formatted);
        }

        free(code_buf.data);
    }

    free(lines);

    // Ensure single newline at end
    if (result.length > 0) {
        // Remove trailing whitespace
        while (result.length > 0 &&
               (result.data[result.length - 1] == ' ' ||
                result.data[result.length - 1] == '\t')) {
            result.length--;
        }
        // Collapse trailing newlines to one
        while (result.length > 1 &&
               result.data[result.length - 1] == '\n' &&
               result.data[result.length - 2] == '\n') {
            result.length--;
        }
        if (result.data[result.length - 1] != '\n') {
            buf_append_char(&result, '\n');
        }
    } else {
        buf_append_char(&result, '\n');
    }

    // Null-terminate
    buf_append_char(&result, '\0');
    result.length--;

    if (out_length) {
        *out_length = result.length;
    }

    return result.data;
}
