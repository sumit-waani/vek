/*
 * vek compiler - Single-pass Pratt parser that compiles directly to bytecode.
 *
 * Follows the Crafting Interpreters pattern:
 * - Pratt parser with precedence climbing for expressions
 * - Locals accessed by stack slot index
 * - Upvalues for closures (captured by reference)
 * - Newlines act as statement terminators (like Ruby)
 * - `end` closes blocks (if, while, for, fn, etc.)
 */

#include "compiler.h"
#include "memory.h"
#include "gc.h"

#include <errno.h>
#include <math.h>

// ---- Parser state ----

typedef struct {
    Token current;
    Token previous;
    Lexer* lexer;
    bool had_error;
    bool panic_mode;
} Parser;

// ---- Precedence levels ----

typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT,    // =
    PREC_OR,            // || or
    PREC_AND,           // && and
    PREC_BITWISE_OR,    // |
    PREC_BITWISE_XOR,   // ^
    PREC_BITWISE_AND,   // &
    PREC_EQUALITY,      // == !=
    PREC_COMPARISON,    // < > <= >=
    PREC_SHIFT,         // << >>
    PREC_RANGE,         // .. ...
    PREC_TERM,          // + -
    PREC_FACTOR,        // * / %
    PREC_POWER,         // **
    PREC_UNARY,         // ! - ~
    PREC_CALL,          // . () []
    PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(bool can_assign);

typedef struct {
    ParseFn prefix;
    ParseFn infix;
    Precedence precedence;
} ParseRule;

// ---- Local variable tracking ----

typedef struct {
    Token name;
    int depth;
    bool is_captured;
} Local;

typedef struct {
    uint8_t index;
    bool is_local;
} Upvalue;

#define MAX_LOCALS 256
#define MAX_UPVALUES 256

typedef enum {
    TYPE_FUNCTION,
    TYPE_SCRIPT,
} FunctionType;

// ---- Compiler state (one per function being compiled) ----

typedef struct Compiler {
    struct Compiler* enclosing;
    ObjFunction* function;
    FunctionType type;

    Local locals[MAX_LOCALS];
    int local_count;
    Upvalue upvalues[MAX_UPVALUES];
    int scope_depth;
} Compiler;

// ---- Loop tracking for break/next ----

typedef struct Loop {
    struct Loop* enclosing;
    int start;          // offset to loop back to (for next/continue)
    int body;           // offset of the body start
    int exit_jump;      // patch point for break (we use a small array)
    int break_count;
    int breaks[256];    // break jump offsets to patch
    int scope_depth;
} Loop;

// ---- Global state ----

static Parser parser;
static Compiler* current = NULL;
static Loop* current_loop = NULL;
static bool created_local = false; // set when assignment creates a new local

// ---- Forward declarations ----
static void expression(void);
static void statement(void);
static void declaration(void);
static ParseRule* get_rule(TokenType type);
static void parse_precedence(Precedence precedence);

// ---- Chunk helpers ----

static Chunk* current_chunk(void) {
    return &current->function->chunk;
}

// ---- Error reporting ----

static void error_at(Token* token, const char* message) {
    if (parser.panic_mode) return;
    parser.panic_mode = true;

    fprintf(stderr, "[line %d] Error", token->line);

    if (token->type == TOKEN_EOF) {
        fprintf(stderr, " at end");
    } else if (token->type != TOKEN_ERROR) {
        fprintf(stderr, " at '%.*s'", token->length, token->start);
    }

    fprintf(stderr, ": %s\n", message);
    parser.had_error = true;
}

static void error(const char* message) {
    error_at(&parser.previous, message);
}

static void error_at_current(const char* message) {
    error_at(&parser.current, message);
}

// ---- Token scanning ----

static void advance(void) {
    parser.previous = parser.current;

    for (;;) {
        parser.current = lexer_next_token(parser.lexer);
        if (parser.current.type != TOKEN_ERROR) break;
        error_at_current(parser.current.start);
    }
}

static void consume(TokenType type, const char* message) {
    if (parser.current.type == type) {
        advance();
        return;
    }
    error_at_current(message);
}

static bool check(TokenType type) {
    return parser.current.type == type;
}

static bool match(TokenType type) {
    if (!check(type)) return false;
    advance();
    return true;
}

// Skip insignificant newlines
static void skip_newlines(void) {
    while (match(TOKEN_NEWLINE)) {
        // consume newlines
    }
}

// ---- Emit bytecode ----

static void emit_byte(uint8_t byte) {
    chunk_write(current_chunk(), byte, parser.previous.line);
}

static void emit_bytes(uint8_t byte1, uint8_t byte2) {
    emit_byte(byte1);
    emit_byte(byte2);
}

static void emit_short(uint16_t value) {
    emit_byte((uint8_t)(value & 0xFF));
    emit_byte((uint8_t)((value >> 8) & 0xFF));
}

static void emit_return(void) {
    emit_byte(OP_NIL);
    emit_byte(OP_RETURN);
}

static int emit_jump(uint8_t instruction) {
    emit_byte(instruction);
    emit_byte(0xFF);
    emit_byte(0xFF);
    return current_chunk()->count - 2;
}

static void patch_jump(int offset) {
    // -2 to adjust for the jump offset itself
    int jump = current_chunk()->count - offset - 2;

    if (jump > 65535) {
        error("Too much code to jump over.");
    }

    current_chunk()->code[offset] = (uint8_t)(jump & 0xFF);
    current_chunk()->code[offset + 1] = (uint8_t)((jump >> 8) & 0xFF);
}

static void emit_loop(int loop_start) {
    emit_byte(OP_LOOP);

    int offset = current_chunk()->count - loop_start + 2;
    if (offset > 65535) error("Loop body too large.");

    emit_byte((uint8_t)(offset & 0xFF));
    emit_byte((uint8_t)((offset >> 8) & 0xFF));
}

static uint16_t make_constant(Value value) {
    int constant = chunk_add_constant(current_chunk(), value);
    if (constant > 65535) {
        error("Too many constants in one chunk.");
        return 0;
    }
    return (uint16_t)constant;
}

static void emit_constant(Value value) {
    uint16_t constant = make_constant(value);
    emit_byte(OP_CONSTANT);
    emit_short(constant);
}

// ---- Compiler initialization ----

static void init_compiler(Compiler* compiler, FunctionType type) {
    compiler->enclosing = current;
    compiler->function = NULL;
    compiler->type = type;
    compiler->local_count = 0;
    compiler->scope_depth = 0;
    compiler->function = obj_function_new();
    current = compiler;

    if (type != TYPE_SCRIPT) {
        current->function->name = obj_string_copy(
            parser.previous.start, (uint32_t)parser.previous.length);
    }

    // Slot 0 is reserved for the function itself (or empty string for scripts)
    Local* local = &current->locals[current->local_count++];
    local->depth = 0;
    local->is_captured = false;
    local->name.start = "";
    local->name.length = 0;
}

static ObjFunction* end_compiler(void) {
    emit_return();
    ObjFunction* function = current->function;

#ifdef VEK_DEBUG
    if (!parser.had_error) {
        #include "debug.h"
        disassemble_chunk(current_chunk(),
            function->name != NULL ? function->name->data : "<script>");
    }
#endif

    current = current->enclosing;
    return function;
}

// ---- Scope management ----

static void begin_scope(void) {
    current->scope_depth++;
}

static void end_scope(void) {
    current->scope_depth--;

    while (current->local_count > 0 &&
           current->locals[current->local_count - 1].depth > current->scope_depth) {
        if (current->locals[current->local_count - 1].is_captured) {
            emit_byte(OP_CLOSE_UPVALUE);
        } else {
            emit_byte(OP_POP);
        }
        current->local_count--;
    }
}

// ---- Variable resolution ----

static uint16_t identifier_constant(Token* name) {
    ObjString* str = obj_string_copy(name->start, (uint32_t)name->length);
    return make_constant(OBJ_VAL(str));
}

static bool identifiers_equal(Token* a, Token* b) {
    if (a->length != b->length) return false;
    return memcmp(a->start, b->start, (size_t)a->length) == 0;
}

static int resolve_local(Compiler* compiler, Token* name) {
    for (int i = compiler->local_count - 1; i >= 0; i--) {
        Local* local = &compiler->locals[i];
        if (identifiers_equal(name, &local->name)) {
            if (local->depth == -1) {
                error("Cannot read local variable in its own initializer.");
            }
            return i;
        }
    }
    return -1;
}

static int add_upvalue(Compiler* compiler, uint8_t index, bool is_local) {
    int upvalue_count = compiler->function->upvalue_count;

    // Check if we already have this upvalue
    for (int i = 0; i < upvalue_count; i++) {
        Upvalue* upvalue = &compiler->upvalues[i];
        if (upvalue->index == index && upvalue->is_local == is_local) {
            return i;
        }
    }

    if (upvalue_count >= MAX_UPVALUES) {
        error("Too many closure variables in function.");
        return 0;
    }

    compiler->upvalues[upvalue_count].is_local = is_local;
    compiler->upvalues[upvalue_count].index = index;
    return compiler->function->upvalue_count++;
}

static int resolve_upvalue(Compiler* compiler, Token* name) {
    if (compiler->enclosing == NULL) return -1;

    int local = resolve_local(compiler->enclosing, name);
    if (local != -1) {
        compiler->enclosing->locals[local].is_captured = true;
        return add_upvalue(compiler, (uint8_t)local, true);
    }

    int upvalue = resolve_upvalue(compiler->enclosing, name);
    if (upvalue != -1) {
        return add_upvalue(compiler, (uint8_t)upvalue, false);
    }

    return -1;
}

static void add_local(Token name) {
    if (current->local_count >= MAX_LOCALS) {
        error("Too many local variables in function.");
        return;
    }

    Local* local = &current->locals[current->local_count++];
    local->name = name;
    local->depth = -1;  // uninitialized
    local->is_captured = false;
}

static void declare_variable(void) {
    if (current->scope_depth == 0) return;

    Token* name = &parser.previous;

    // Check for redeclaration in same scope
    for (int i = current->local_count - 1; i >= 0; i--) {
        Local* local = &current->locals[i];
        if (local->depth != -1 && local->depth < current->scope_depth) {
            break;
        }
        if (identifiers_equal(name, &local->name)) {
            error("Already a variable with this name in this scope.");
        }
    }

    add_local(*name);
}

static uint16_t parse_variable(const char* error_message) {
    consume(TOKEN_IDENTIFIER, error_message);

    declare_variable();
    if (current->scope_depth > 0) return 0;

    return identifier_constant(&parser.previous);
}

static void mark_initialized(void) {
    if (current->scope_depth == 0) return;
    current->locals[current->local_count - 1].depth = current->scope_depth;
}

static void define_variable(uint16_t global) {
    if (current->scope_depth > 0) {
        mark_initialized();
        return;
    }
    emit_byte(OP_DEFINE_GLOBAL);
    emit_short(global);
}

// ---- Named variable get/set ----

static void named_variable(Token name, bool can_assign) {
    uint8_t get_op, set_op;
    int arg = resolve_local(current, &name);

    if (arg != -1) {
        get_op = OP_GET_LOCAL;
        set_op = OP_SET_LOCAL;
    } else if ((arg = resolve_upvalue(current, &name)) != -1) {
        get_op = OP_GET_UPVALUE;
        set_op = OP_SET_UPVALUE;
    } else {
        arg = (int)identifier_constant(&name);
        get_op = OP_GET_GLOBAL;
        set_op = OP_SET_GLOBAL;
    }

    if (can_assign && match(TOKEN_EQUAL)) {
        skip_newlines();
        expression();

        // If this is a new variable (not resolved as local/upvalue):
        if (get_op == OP_GET_GLOBAL) {
            // Check if we're in a local scope; if so, declare a new local
            if (current->scope_depth > 0) {
                // Create a new local variable - value is already on stack
                // in the correct slot position; no SET_LOCAL needed.
                add_local(name);
                mark_initialized();
                created_local = true;
            } else {
                // At global scope, set/create a global (SET_GLOBAL peeks, not pops,
                // so expression_statement's OP_POP will clean up)
                emit_byte(OP_SET_GLOBAL);
                emit_short((uint16_t)arg);
            }
        } else if (set_op == OP_SET_UPVALUE) {
            emit_bytes(set_op, (uint8_t)arg);
        } else {
            emit_byte(set_op);
            emit_short((uint16_t)arg);
        }
    } else {
        if (get_op == OP_GET_UPVALUE) {
            emit_bytes(get_op, (uint8_t)arg);
        } else {
            emit_byte(get_op);
            emit_short((uint16_t)arg);
        }
    }
}

// ---- Expression parsing (prefix/infix rules) ----

static void number(bool can_assign) {
    (void)can_assign;
    // Check if integer or float
    bool is_float = false;
    for (int i = 0; i < parser.previous.length; i++) {
        char c = parser.previous.start[i];
        if (c == '.' || c == 'e' || c == 'E') {
            is_float = true;
            break;
        }
    }

    if (is_float) {
        double value = strtod(parser.previous.start, NULL);
        emit_constant(FLOAT_VAL(value));
    } else {
        // Parse integer, handling underscores
        int64_t value = 0;
        const char* p = parser.previous.start;
        const char* end = p + parser.previous.length;
        bool negative = false;

        if (*p == '-') {
            negative = true;
            p++;
        }

        while (p < end) {
            if (*p == '_') { p++; continue; }
            value = value * 10 + (*p - '0');
            p++;
        }

        if (negative) value = -value;
        emit_constant(INT_VAL(value));
    }
}

static void string_literal(bool can_assign) {
    (void)can_assign;
    // Strip the surrounding quotes
    const char* start = parser.previous.start + 1;
    int length = parser.previous.length - 2;

    // Handle escape sequences
    // For simplicity in Phase 1, allocate a buffer and process escapes
    char* buffer = (char*)malloc((size_t)length + 1);
    int buf_len = 0;

    for (int i = 0; i < length; i++) {
        if (start[i] == '\\' && i + 1 < length) {
            i++;
            switch (start[i]) {
                case 'n':  buffer[buf_len++] = '\n'; break;
                case 't':  buffer[buf_len++] = '\t'; break;
                case 'r':  buffer[buf_len++] = '\r'; break;
                case '\\': buffer[buf_len++] = '\\'; break;
                case '"':  buffer[buf_len++] = '"';  break;
                case '\'': buffer[buf_len++] = '\''; break;
                case '0':  buffer[buf_len++] = '\0'; break;
                default:   buffer[buf_len++] = start[i]; break;
            }
        } else {
            buffer[buf_len++] = start[i];
        }
    }

    ObjString* str = obj_string_copy(buffer, (uint32_t)buf_len);
    free(buffer);
    emit_constant(OBJ_VAL(str));
}

static void symbol_literal(bool can_assign) {
    (void)can_assign;
    // Strip the leading : from :name
    const char* start = parser.previous.start + 1;
    int length = parser.previous.length - 1;
    ObjString* str = obj_string_copy(start, (uint32_t)length);
    emit_constant(OBJ_VAL(str));
}

static void literal(bool can_assign) {
    (void)can_assign;
    switch (parser.previous.type) {
        case TOKEN_NIL:   emit_byte(OP_NIL); break;
        case TOKEN_TRUE:  emit_byte(OP_TRUE); break;
        case TOKEN_FALSE: emit_byte(OP_FALSE); break;
        default: return; // Unreachable
    }
}

static void variable(bool can_assign) {
    named_variable(parser.previous, can_assign);
}

static void grouping(bool can_assign) {
    (void)can_assign;
    skip_newlines();
    expression();
    skip_newlines();
    consume(TOKEN_RPAREN, "Expected ')' after expression.");
}

static void unary(bool can_assign) {
    (void)can_assign;
    TokenType operator_type = parser.previous.type;

    // Compile the operand
    parse_precedence(PREC_UNARY);

    switch (operator_type) {
        case TOKEN_MINUS: emit_byte(OP_NEG); break;
        case TOKEN_BANG:
        case TOKEN_NOT:   emit_byte(OP_NOT); break;
        case TOKEN_TILDE: emit_byte(OP_BNOT); break;
        default: return; // Unreachable
    }
}

static void binary(bool can_assign) {
    (void)can_assign;
    TokenType operator_type = parser.previous.type;
    ParseRule* rule = get_rule(operator_type);
    parse_precedence((Precedence)(rule->precedence + 1));

    switch (operator_type) {
        case TOKEN_PLUS:          emit_byte(OP_ADD); break;
        case TOKEN_MINUS:         emit_byte(OP_SUB); break;
        case TOKEN_STAR:          emit_byte(OP_MUL); break;
        case TOKEN_SLASH:         emit_byte(OP_DIV); break;
        case TOKEN_PERCENT:       emit_byte(OP_MOD); break;
        case TOKEN_STAR_STAR:     emit_byte(OP_POWER); break;
        case TOKEN_EQUAL_EQUAL:   emit_byte(OP_EQUAL); break;
        case TOKEN_BANG_EQUAL:    emit_byte(OP_NOT_EQUAL); break;
        case TOKEN_LESS:          emit_byte(OP_LESS); break;
        case TOKEN_LESS_EQUAL:    emit_byte(OP_LESS_EQUAL); break;
        case TOKEN_GREATER:       emit_byte(OP_GREATER); break;
        case TOKEN_GREATER_EQUAL: emit_byte(OP_GREATER_EQUAL); break;
        case TOKEN_AMP:           emit_byte(OP_BAND); break;
        case TOKEN_PIPE:          emit_byte(OP_BOR); break;
        case TOKEN_CARET:         emit_byte(OP_BXOR); break;
        case TOKEN_LSHIFT:        emit_byte(OP_SHL); break;
        case TOKEN_RSHIFT:        emit_byte(OP_SHR); break;
        default: return; // Unreachable
    }
}

static void and_(bool can_assign) {
    (void)can_assign;
    int end_jump = emit_jump(OP_JUMP_IF_FALSE);

    emit_byte(OP_POP);
    skip_newlines();
    parse_precedence(PREC_AND);

    patch_jump(end_jump);
}

static void or_(bool can_assign) {
    (void)can_assign;
    int else_jump = emit_jump(OP_JUMP_IF_FALSE);
    int end_jump = emit_jump(OP_JUMP);

    patch_jump(else_jump);
    emit_byte(OP_POP);

    skip_newlines();
    parse_precedence(PREC_OR);

    patch_jump(end_jump);
}

static uint8_t argument_list(void) {
    uint8_t arg_count = 0;
    if (!check(TOKEN_RPAREN)) {
        do {
            skip_newlines();
            expression();
            if (arg_count == 255) {
                error("Cannot have more than 255 arguments.");
            }
            arg_count++;
            skip_newlines();
        } while (match(TOKEN_COMMA));
    }
    skip_newlines();
    consume(TOKEN_RPAREN, "Expected ')' after arguments.");
    return arg_count;
}

static void call(bool can_assign) {
    (void)can_assign;
    uint8_t arg_count = argument_list();
    emit_bytes(OP_CALL, arg_count);
}

static bool is_identifier_like(TokenType type) {
    // Allow keywords as field/method names after '.'
    // All keyword tokens are between TOKEN_FN and TOKEN_HALT (inclusive).
    // Rather than enumerating each one, use a range check so new keywords
    // added to the lexer are automatically allowed without updating this.
    return type == TOKEN_IDENTIFIER ||
           (type >= TOKEN_FN && type <= TOKEN_HALT);
}

static void dot(bool can_assign) {
    if (!is_identifier_like(parser.current.type)) {
        error_at_current("Expected property name after '.'.");
        return;
    }
    advance();
    uint16_t name = identifier_constant(&parser.previous);

    if (can_assign && match(TOKEN_EQUAL)) {
        skip_newlines();
        expression();
        emit_byte(OP_SET_FIELD);
        emit_short(name);
    } else if (match(TOKEN_LPAREN)) {
        // Method call: obj.method(args)
        // First get the method (object is on stack top, GET_FIELD pops it and pushes method)
        emit_byte(OP_GET_FIELD);
        emit_short(name);
        // Now push arguments
        uint8_t arg_count = argument_list();
        emit_bytes(OP_CALL, arg_count);
    } else {
        emit_byte(OP_GET_FIELD);
        emit_short(name);
    }
}

static void index_expr(bool can_assign) {
    (void)can_assign;
    skip_newlines();
    expression();
    skip_newlines();
    consume(TOKEN_RBRACKET, "Expected ']' after index.");

    if (can_assign && match(TOKEN_EQUAL)) {
        skip_newlines();
        expression();
        emit_byte(OP_SET_INDEX);
    } else {
        emit_byte(OP_GET_INDEX);
    }
}

static void list_literal(bool can_assign) {
    (void)can_assign;
    int count = 0;
    skip_newlines();
    if (!check(TOKEN_RBRACKET)) {
        do {
            skip_newlines();
            expression();
            count++;
            skip_newlines();
        } while (match(TOKEN_COMMA));
    }
    skip_newlines();
    consume(TOKEN_RBRACKET, "Expected ']' after list elements.");
    emit_byte(OP_NEW_LIST);
    emit_short((uint16_t)count);
}

static void map_literal(bool can_assign) {
    (void)can_assign;
    int count = 0;
    skip_newlines();
    if (!check(TOKEN_RBRACE)) {
        do {
            skip_newlines();
            // Key can be an identifier (shorthand for string key) or expression
            if (check(TOKEN_IDENTIFIER) || check(TOKEN_STRING)) {
                if (check(TOKEN_IDENTIFIER)) {
                    advance();
                    // Use identifier as string key
                    ObjString* key = obj_string_copy(
                        parser.previous.start, (uint32_t)parser.previous.length);
                    emit_constant(OBJ_VAL(key));
                } else {
                    advance();
                    string_literal(false);
                }
            } else {
                expression();
            }
            consume(TOKEN_COLON, "Expected ':' after map key.");
            skip_newlines();
            expression();
            count++;
            skip_newlines();
        } while (match(TOKEN_COMMA));
    }
    skip_newlines();
    consume(TOKEN_RBRACE, "Expected '}' after map entries.");
    emit_byte(OP_NEW_MAP);
    emit_short((uint16_t)count);
}

// ---- Parse rules table ----

static ParseRule rules[TOKEN_COUNT];

static void init_rules(void) {
    // Initialize all to {NULL, NULL, PREC_NONE}
    for (int i = 0; i < TOKEN_COUNT; i++) {
        rules[i] = (ParseRule){NULL, NULL, PREC_NONE};
    }

    // Prefix rules
    rules[TOKEN_LPAREN]     = (ParseRule){grouping, call,   PREC_CALL};
    rules[TOKEN_LBRACKET]   = (ParseRule){list_literal, index_expr, PREC_CALL};
    rules[TOKEN_LBRACE]     = (ParseRule){map_literal, NULL, PREC_NONE};
    rules[TOKEN_MINUS]      = (ParseRule){unary, binary, PREC_TERM};
    rules[TOKEN_PLUS]       = (ParseRule){NULL,  binary, PREC_TERM};
    rules[TOKEN_BANG]       = (ParseRule){unary, NULL,   PREC_NONE};
    rules[TOKEN_NOT]        = (ParseRule){unary, NULL,   PREC_NONE};
    rules[TOKEN_TILDE]      = (ParseRule){unary, NULL,   PREC_NONE};

    // Infix rules
    rules[TOKEN_SLASH]         = (ParseRule){NULL, binary, PREC_FACTOR};
    rules[TOKEN_STAR]          = (ParseRule){NULL, binary, PREC_FACTOR};
    rules[TOKEN_PERCENT]       = (ParseRule){NULL, binary, PREC_FACTOR};
    rules[TOKEN_STAR_STAR]     = (ParseRule){NULL, binary, PREC_POWER};
    rules[TOKEN_EQUAL_EQUAL]   = (ParseRule){NULL, binary, PREC_EQUALITY};
    rules[TOKEN_BANG_EQUAL]    = (ParseRule){NULL, binary, PREC_EQUALITY};
    rules[TOKEN_LESS]          = (ParseRule){NULL, binary, PREC_COMPARISON};
    rules[TOKEN_LESS_EQUAL]    = (ParseRule){NULL, binary, PREC_COMPARISON};
    rules[TOKEN_GREATER]       = (ParseRule){NULL, binary, PREC_COMPARISON};
    rules[TOKEN_GREATER_EQUAL] = (ParseRule){NULL, binary, PREC_COMPARISON};
    rules[TOKEN_AMP]           = (ParseRule){NULL, binary, PREC_BITWISE_AND};
    rules[TOKEN_PIPE]          = (ParseRule){NULL, binary, PREC_BITWISE_OR};
    rules[TOKEN_CARET]         = (ParseRule){NULL, binary, PREC_BITWISE_XOR};
    rules[TOKEN_LSHIFT]        = (ParseRule){NULL, binary, PREC_SHIFT};
    rules[TOKEN_RSHIFT]        = (ParseRule){NULL, binary, PREC_SHIFT};
    rules[TOKEN_AMP_AMP]       = (ParseRule){NULL, and_,   PREC_AND};
    rules[TOKEN_PIPE_PIPE]     = (ParseRule){NULL, or_,    PREC_OR};
    rules[TOKEN_AND]           = (ParseRule){NULL, and_,   PREC_AND};
    rules[TOKEN_OR]            = (ParseRule){NULL, or_,    PREC_OR};
    rules[TOKEN_DOT]           = (ParseRule){NULL, dot,    PREC_CALL};

    // Literals
    rules[TOKEN_INT]        = (ParseRule){number,         NULL, PREC_NONE};
    rules[TOKEN_FLOAT]      = (ParseRule){number,         NULL, PREC_NONE};
    rules[TOKEN_STRING]     = (ParseRule){string_literal, NULL, PREC_NONE};
    rules[TOKEN_SYMBOL]     = (ParseRule){symbol_literal, NULL, PREC_NONE};
    rules[TOKEN_IDENTIFIER] = (ParseRule){variable,       NULL, PREC_NONE};
    rules[TOKEN_NIL]        = (ParseRule){literal,        NULL, PREC_NONE};
    rules[TOKEN_TRUE]       = (ParseRule){literal,        NULL, PREC_NONE};
    rules[TOKEN_FALSE]      = (ParseRule){literal,        NULL, PREC_NONE};
}

static ParseRule* get_rule(TokenType type) {
    return &rules[type];
}

// ---- Pratt parser core ----

static void parse_precedence(Precedence precedence) {
    skip_newlines();
    advance();
    ParseFn prefix_rule = get_rule(parser.previous.type)->prefix;
    if (prefix_rule == NULL) {
        error("Expected expression.");
        return;
    }

    bool can_assign = (precedence <= PREC_ASSIGNMENT);
    prefix_rule(can_assign);

    while (precedence <= get_rule(parser.current.type)->precedence) {
        advance();
        ParseFn infix_rule = get_rule(parser.previous.type)->infix;
        if (infix_rule != NULL) {
            infix_rule(can_assign);
        }
    }

    if (can_assign && match(TOKEN_EQUAL)) {
        error("Invalid assignment target.");
    }
}

static void expression(void) {
    parse_precedence(PREC_ASSIGNMENT);
}

// ---- Statement compilation ----

static void expression_statement(void) {
    created_local = false;
    expression();
    if (!created_local) {
        emit_byte(OP_POP);
    }
    created_local = false;
}

static void if_statement(void) {
    skip_newlines();
    expression();

    // Optional 'then' keyword
    skip_newlines();
    match(TOKEN_THEN);
    skip_newlines();

    int then_jump = emit_jump(OP_JUMP_IF_FALSE);
    emit_byte(OP_POP);  // pop condition

    // Then branch
    while (!check(TOKEN_ELSIF) && !check(TOKEN_ELSE) &&
           !check(TOKEN_END) && !check(TOKEN_EOF)) {
        declaration();
        skip_newlines();
    }

    int else_jump = emit_jump(OP_JUMP);
    patch_jump(then_jump);
    emit_byte(OP_POP);  // pop condition

    // Collect all exit jumps to patch at the end
    int exit_jumps[256];
    int exit_count = 0;
    exit_jumps[exit_count++] = else_jump;

    // Handle elsif chain
    while (match(TOKEN_ELSIF)) {
        skip_newlines();
        expression();
        skip_newlines();
        match(TOKEN_THEN);
        skip_newlines();

        int elsif_jump = emit_jump(OP_JUMP_IF_FALSE);
        emit_byte(OP_POP);

        while (!check(TOKEN_ELSIF) && !check(TOKEN_ELSE) &&
               !check(TOKEN_END) && !check(TOKEN_EOF)) {
            declaration();
            skip_newlines();
        }

        int next_jump = emit_jump(OP_JUMP);
        exit_jumps[exit_count++] = next_jump;
        patch_jump(elsif_jump);
        emit_byte(OP_POP);
    }

    // Else branch
    if (match(TOKEN_ELSE)) {
        skip_newlines();
        while (!check(TOKEN_END) && !check(TOKEN_EOF)) {
            declaration();
            skip_newlines();
        }
    }

    // Patch all exit jumps to here (end of if/elsif/else)
    for (int i = 0; i < exit_count; i++) {
        patch_jump(exit_jumps[i]);
    }

    skip_newlines();
    consume(TOKEN_END, "Expected 'end' after if statement.");
}

static void while_statement(void) {
    Loop loop;
    loop.enclosing = current_loop;
    loop.start = current_chunk()->count;
    loop.break_count = 0;
    loop.scope_depth = current->scope_depth;
    current_loop = &loop;

    skip_newlines();
    expression();
    skip_newlines();

    loop.exit_jump = emit_jump(OP_JUMP_IF_FALSE);
    emit_byte(OP_POP);  // pop condition
    loop.body = current_chunk()->count;

    // Body
    while (!check(TOKEN_END) && !check(TOKEN_EOF)) {
        declaration();
        skip_newlines();
    }

    emit_loop(loop.start);
    patch_jump(loop.exit_jump);
    emit_byte(OP_POP);  // pop condition

    // Patch breaks
    for (int i = 0; i < loop.break_count; i++) {
        patch_jump(loop.breaks[i]);
    }

    consume(TOKEN_END, "Expected 'end' after while loop.");
    current_loop = loop.enclosing;
}

static void until_statement(void) {
    Loop loop;
    loop.enclosing = current_loop;
    loop.start = current_chunk()->count;
    loop.break_count = 0;
    loop.scope_depth = current->scope_depth;
    current_loop = &loop;

    skip_newlines();
    expression();
    skip_newlines();

    // until = while NOT condition
    // Jump if TRUE (condition met means stop)
    // But we only have JUMP_IF_FALSE, so: if condition is truthy, jump out
    // JUMP_IF_FALSE jumps when falsy. We want to jump when truthy.
    // Solution: emit NOT + JUMP_IF_FALSE
    emit_byte(OP_NOT);
    loop.exit_jump = emit_jump(OP_JUMP_IF_FALSE);
    emit_byte(OP_POP);
    loop.body = current_chunk()->count;

    while (!check(TOKEN_END) && !check(TOKEN_EOF)) {
        declaration();
        skip_newlines();
    }

    emit_loop(loop.start);
    patch_jump(loop.exit_jump);
    emit_byte(OP_POP);

    for (int i = 0; i < loop.break_count; i++) {
        patch_jump(loop.breaks[i]);
    }

    consume(TOKEN_END, "Expected 'end' after until loop.");
    current_loop = loop.enclosing;
}

static void loop_statement(void) {
    // loop do ... end
    match(TOKEN_DO);
    skip_newlines();

    Loop loop;
    loop.enclosing = current_loop;
    loop.start = current_chunk()->count;
    loop.break_count = 0;
    loop.scope_depth = current->scope_depth;
    current_loop = &loop;

    while (!check(TOKEN_END) && !check(TOKEN_EOF)) {
        declaration();
        skip_newlines();
    }

    emit_loop(loop.start);

    for (int i = 0; i < loop.break_count; i++) {
        patch_jump(loop.breaks[i]);
    }

    consume(TOKEN_END, "Expected 'end' after loop.");
    current_loop = loop.enclosing;
}

static void for_statement(void) {
    // for <var> in <iterable> ... end
    begin_scope();

    Loop loop;
    loop.enclosing = current_loop;
    loop.break_count = 0;
    loop.scope_depth = current->scope_depth;
    current_loop = &loop;

    skip_newlines();
    consume(TOKEN_IDENTIFIER, "Expected variable name after 'for'.");
    Token var_name = parser.previous;
    add_local(var_name);
    mark_initialized();
    int var_slot = current->local_count - 1;

    // Push placeholder for the loop variable (it occupies a stack slot)
    emit_byte(OP_NIL);

    consume(TOKEN_IN, "Expected 'in' after for variable.");
    skip_newlines();
    expression();
    skip_newlines();

    // Create iterator (hidden local)
    emit_byte(OP_ITER_INIT);
    Token iter_token = {TOKEN_IDENTIFIER, "", 0, 0};
    add_local(iter_token);
    mark_initialized();
    int iter_slot = current->local_count - 1;

    // Iterator is now on stack in the correct slot position

    loop.start = current_chunk()->count;

    // Get next value from iterator; jump to end if done
    emit_byte(OP_GET_LOCAL);
    emit_short((uint16_t)iter_slot);

    int exit_jump = emit_jump(OP_ITER_NEXT);

    // Store the yielded value in the loop variable
    emit_byte(OP_SET_LOCAL);
    emit_short((uint16_t)var_slot);
    emit_byte(OP_POP);

    loop.body = current_chunk()->count;

    // Body
    while (!check(TOKEN_END) && !check(TOKEN_EOF)) {
        declaration();
        skip_newlines();
    }

    emit_loop(loop.start);
    patch_jump(exit_jump);

    for (int i = 0; i < loop.break_count; i++) {
        patch_jump(loop.breaks[i]);
    }

    consume(TOKEN_END, "Expected 'end' after for loop.");
    end_scope();
    current_loop = loop.enclosing;
}

static void return_statement(void) {
    if (current->type == TYPE_SCRIPT) {
        error("Cannot return from top-level code.");
    }

    if (check(TOKEN_NEWLINE) || check(TOKEN_EOF) || check(TOKEN_END)) {
        emit_return();
    } else {
        expression();
        emit_byte(OP_RETURN);
    }
}

static void break_statement(void) {
    if (current_loop == NULL) {
        error("Cannot use 'break' outside of a loop.");
        return;
    }

    // Close any locals in scopes between here and the loop scope
    int depth = current->scope_depth;
    while (depth > current_loop->scope_depth) {
        for (int i = current->local_count - 1; i >= 0; i--) {
            if (current->locals[i].depth <= depth - 1) break;
            if (current->locals[i].is_captured) {
                emit_byte(OP_CLOSE_UPVALUE);
            } else {
                emit_byte(OP_POP);
            }
        }
        depth--;
    }

    if (current_loop->break_count >= 256) {
        error("Too many break statements in loop.");
        return;
    }
    current_loop->breaks[current_loop->break_count++] = emit_jump(OP_JUMP);
}

static void next_statement(void) {
    if (current_loop == NULL) {
        error("Cannot use 'next' outside of a loop.");
        return;
    }

    // Close any locals in scopes between here and the loop scope
    int depth = current->scope_depth;
    while (depth > current_loop->scope_depth) {
        for (int i = current->local_count - 1; i >= 0; i--) {
            if (current->locals[i].depth <= depth - 1) break;
            if (current->locals[i].is_captured) {
                emit_byte(OP_CLOSE_UPVALUE);
            } else {
                emit_byte(OP_POP);
            }
        }
        depth--;
    }

    emit_loop(current_loop->start);
}

static void fn_declaration(void) {
    uint16_t global = parse_variable("Expected function name.");
    mark_initialized();

    // Compile function body
    Compiler compiler;
    init_compiler(&compiler, TYPE_FUNCTION);
    begin_scope();

    // Parameters
    consume(TOKEN_LPAREN, "Expected '(' after function name.");
    if (!check(TOKEN_RPAREN)) {
        do {
            skip_newlines();
            current->function->arity++;
            if (current->function->arity > 255) {
                error_at_current("Cannot have more than 255 parameters.");
            }
            uint16_t param = parse_variable("Expected parameter name.");
            define_variable(param);
            skip_newlines();
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RPAREN, "Expected ')' after parameters.");
    skip_newlines();

    // Body
    while (!check(TOKEN_END) && !check(TOKEN_EOF)) {
        declaration();
        skip_newlines();
    }

    // The last expression is the implicit return in vek (like Ruby)
    // For now, the end_compiler emits OP_NIL + OP_RETURN

    ObjFunction* function = end_compiler();
    consume(TOKEN_END, "Expected 'end' after function body.");

    // Emit closure
    uint16_t constant = make_constant(OBJ_VAL(function));
    emit_byte(OP_CLOSURE);
    emit_short(constant);

    for (int i = 0; i < function->upvalue_count; i++) {
        emit_byte(compiler.upvalues[i].is_local ? 1 : 0);
        emit_byte(compiler.upvalues[i].index);
    }

    define_variable(global);
}

static void synchronize(void) {
    parser.panic_mode = false;

    while (parser.current.type != TOKEN_EOF) {
        if (parser.previous.type == TOKEN_NEWLINE) return;

        switch (parser.current.type) {
            case TOKEN_FN:
            case TOKEN_IF:
            case TOKEN_WHILE:
            case TOKEN_UNTIL:
            case TOKEN_FOR:
            case TOKEN_LOOP:
            case TOKEN_RETURN:
            case TOKEN_BEGIN:
                return;
            default:
                ; // Keep going
        }

        advance();
    }
}

static void declaration(void) {
    skip_newlines();

    if (check(TOKEN_EOF)) return;

    if (match(TOKEN_FN)) {
        fn_declaration();
    } else {
        statement();
    }

    if (parser.panic_mode) synchronize();
}

static void statement(void) {
    if (match(TOKEN_IF)) {
        if_statement();
    } else if (match(TOKEN_WHILE)) {
        while_statement();
    } else if (match(TOKEN_UNTIL)) {
        until_statement();
    } else if (match(TOKEN_FOR)) {
        for_statement();
    } else if (match(TOKEN_LOOP)) {
        loop_statement();
    } else if (match(TOKEN_RETURN)) {
        return_statement();
    } else if (match(TOKEN_BREAK)) {
        break_statement();
    } else if (match(TOKEN_NEXT)) {
        next_statement();
    } else {
        expression_statement();
    }
}

// ---- Public API ----

ObjFunction* compile(const char* source) {
    // Initialize parse rule table
    init_rules();

    Lexer lexer;
    lexer_init(&lexer, source);
    parser.lexer = &lexer;
    parser.had_error = false;
    parser.panic_mode = false;

    Compiler compiler;
    init_compiler(&compiler, TYPE_SCRIPT);

    advance();

    while (!match(TOKEN_EOF)) {
        declaration();
    }

    ObjFunction* function = end_compiler();
    return parser.had_error ? NULL : function;
}
