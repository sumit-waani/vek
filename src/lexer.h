#ifndef VEK_LEXER_H
#define VEK_LEXER_H

#include "common.h"

// Token types
typedef enum {
    // Single-character tokens
    TOKEN_LPAREN,           // (
    TOKEN_RPAREN,           // )
    TOKEN_LBRACKET,         // [
    TOKEN_RBRACKET,         // ]
    TOKEN_LBRACE,           // {
    TOKEN_RBRACE,           // }
    TOKEN_COMMA,            // ,
    TOKEN_DOT,              // .
    TOKEN_SEMICOLON,        // ;
    TOKEN_COLON,            // :
    TOKEN_PLUS,             // +
    TOKEN_MINUS,            // -
    TOKEN_STAR,             // *
    TOKEN_SLASH,            // /
    TOKEN_PERCENT,          // %
    TOKEN_AMP,              // &
    TOKEN_PIPE,             // |
    TOKEN_CARET,            // ^
    TOKEN_TILDE,            // ~
    TOKEN_LESS,             // <
    TOKEN_GREATER,          // >
    TOKEN_EQUAL,            // =
    TOKEN_BANG,             // !
    TOKEN_AT,               // @
    TOKEN_QUESTION,         // ?

    // Multi-character tokens
    TOKEN_EQUAL_EQUAL,      // ==
    TOKEN_BANG_EQUAL,       // !=
    TOKEN_LESS_EQUAL,       // <=
    TOKEN_GREATER_EQUAL,    // >=
    TOKEN_AMP_AMP,          // &&
    TOKEN_PIPE_PIPE,        // ||
    TOKEN_DOT_DOT,          // ..
    TOKEN_DOT_DOT_DOT,     // ...
    TOKEN_ARROW,            // ->
    TOKEN_FAT_ARROW,       // =>
    TOKEN_SAFE_NAV,         // ?.
    TOKEN_TERNARY,          // ?:
    TOKEN_LSHIFT,           // <<
    TOKEN_RSHIFT,           // >>
    TOKEN_PLUS_EQUAL,       // +=
    TOKEN_MINUS_EQUAL,      // -=
    TOKEN_STAR_EQUAL,       // *=
    TOKEN_SLASH_EQUAL,      // /=
    TOKEN_STAR_STAR,        // **

    // Literals
    TOKEN_INT,              // integer literal
    TOKEN_FLOAT,            // float literal
    TOKEN_STRING,           // string literal
    TOKEN_SYMBOL,           // symbol literal (:name)
    TOKEN_IDENTIFIER,       // identifier

    // Keywords
    TOKEN_FN,
    TOKEN_END,
    TOKEN_DO,
    TOKEN_IF,
    TOKEN_ELSIF,
    TOKEN_ELSE,
    TOKEN_THEN,
    TOKEN_WHILE,
    TOKEN_UNTIL,
    TOKEN_LOOP,
    TOKEN_FOR,
    TOKEN_IN,
    TOKEN_CASE,
    TOKEN_RETURN,
    TOKEN_BREAK,
    TOKEN_NEXT,
    TOKEN_TRUE,
    TOKEN_FALSE,
    TOKEN_NIL,
    TOKEN_AND,
    TOKEN_OR,
    TOKEN_NOT,
    TOKEN_BEGIN,
    TOKEN_RESCUE,
    TOKEN_RAISE,
    TOKEN_UNLESS,
    TOKEN_MODULE,

    // Web keywords
    TOKEN_GET,
    TOKEN_POST,
    TOKEN_PUT,
    TOKEN_PATCH,
    TOKEN_DELETE,
    TOKEN_RENDER,
    TOKEN_REDIRECT,
    TOKEN_HALT,

    // Special tokens
    TOKEN_EOF,
    TOKEN_ERROR,
    TOKEN_NEWLINE,
    TOKEN_INTERPOLATION_START,  // #{

    TOKEN_COUNT  // total number of token types
} TokenType;

// Token structure
typedef struct {
    TokenType type;
    const char* start;
    int length;
    int line;
} Token;

// Lexer state
typedef struct {
    const char* start;      // start of current token
    const char* current;    // current scan position
    int line;               // current line number
} Lexer;

// Initialize the lexer with source code
void lexer_init(Lexer* lexer, const char* source);

// Scan and return the next token
Token lexer_next_token(Lexer* lexer);

#endif // VEK_LEXER_H
