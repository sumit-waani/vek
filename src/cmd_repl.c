#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "cli.h"
#include "lexer.h"
#include "vm.h"
#include "object.h"
#include "gc.h"
#include "memory.h"
#include "vek_stdlib.h"

#include <unistd.h>

// ---- History ----

#define REPL_HISTORY_MAX 100
#define REPL_LINE_MAX    4096
#define REPL_INPUT_MAX   (REPL_LINE_MAX * 64)

static char* repl_history[REPL_HISTORY_MAX];
static int repl_history_count = 0;

static void repl_history_add(const char* input) {
    if (input == NULL || input[0] == '\0') return;

    // Free oldest if at capacity
    if (repl_history_count >= REPL_HISTORY_MAX) {
        free(repl_history[0]);
        memmove(repl_history, repl_history + 1,
                (size_t)(REPL_HISTORY_MAX - 1) * sizeof(char*));
        repl_history_count = REPL_HISTORY_MAX - 1;
    }

    repl_history[repl_history_count] = strdup(input);
    if (repl_history[repl_history_count] != NULL) {
        repl_history_count++;
    }
}

static void repl_history_free(void) {
    for (int i = 0; i < repl_history_count; i++) {
        free(repl_history[i]);
        repl_history[i] = NULL;
    }
    repl_history_count = 0;
}

// ---- Multi-line detection ----

// Returns true if the input is incomplete (needs more lines)
static bool input_is_incomplete(const char* source) {
    Lexer lexer;
    lexer_init(&lexer, source);

    int open_parens = 0;
    int open_brackets = 0;
    int open_braces = 0;
    int block_depth = 0;

    for (;;) {
        Token token = lexer_next_token(&lexer);

        if (token.type == TOKEN_EOF) break;
        if (token.type == TOKEN_ERROR) break;

        switch (token.type) {
            case TOKEN_LPAREN:   open_parens++;   break;
            case TOKEN_RPAREN:   open_parens--;   break;
            case TOKEN_LBRACKET: open_brackets++; break;
            case TOKEN_RBRACKET: open_brackets--; break;
            case TOKEN_LBRACE:   open_braces++;   break;
            case TOKEN_RBRACE:   open_braces--;   break;

            // Block openers
            case TOKEN_FN:
            case TOKEN_IF:
            case TOKEN_WHILE:
            case TOKEN_UNTIL:
            case TOKEN_LOOP:
            case TOKEN_FOR:
            case TOKEN_DO:
            case TOKEN_CASE:
            case TOKEN_BEGIN:
                block_depth++;
                break;

            // Block closer
            case TOKEN_END:
                block_depth--;
                break;

            default:
                break;
        }
    }

    return (open_parens > 0 || open_brackets > 0 ||
            open_braces > 0 || block_depth > 0);
}

// ---- Result printing ----

static void print_value(Value value, bool color) {
    if (IS_NIL(value)) {
        if (color) printf("%snil%s", CLI_CYAN, CLI_RESET);
        else printf("nil");
    } else if (IS_BOOL(value)) {
        const char* s = AS_BOOL(value) ? "true" : "false";
        if (color) printf("%s%s%s", CLI_CYAN, s, CLI_RESET);
        else printf("%s", s);
    } else if (IS_INT(value)) {
        if (color) printf("%s%lld%s", CLI_CYAN, (long long)AS_INT(value), CLI_RESET);
        else printf("%lld", (long long)AS_INT(value));
    } else if (IS_FLOAT(value)) {
        if (color) printf("%s%g%s", CLI_CYAN, AS_DOUBLE(value), CLI_RESET);
        else printf("%g", AS_DOUBLE(value));
    } else if (IS_STRING(value)) {
        ObjString* str = AS_STRING(value);
        if (color) printf("%s\"%.*s\"%s", CLI_CYAN, (int)str->length, str->data, CLI_RESET);
        else printf("\"%.*s\"", (int)str->length, str->data);
    } else if (IS_LIST(value)) {
        ObjList* list = AS_LIST(value);
        if (color) printf("%s[%s", CLI_CYAN, CLI_RESET);
        else printf("[");
        for (uint32_t i = 0; i < list->length; i++) {
            if (i > 0) printf(", ");
            print_value(list->data[i], color);
        }
        if (color) printf("%s]%s", CLI_CYAN, CLI_RESET);
        else printf("]");
    } else if (IS_MAP(value)) {
        ObjMap* map = AS_MAP(value);
        if (color) printf("%s{%s", CLI_CYAN, CLI_RESET);
        else printf("{");
        bool first = true;
        for (uint32_t i = 0; i < map->capacity; i++) {
            if (map->entries[i].key == NULL) continue;
            if (map->entries[i].key == MAP_TOMBSTONE) continue;
            if (!first) printf(", ");
            first = false;
            if (color) {
                printf("%s%.*s%s: ", CLI_CYAN, (int)map->entries[i].key->length,
                       map->entries[i].key->data, CLI_RESET);
            } else {
                printf("%.*s: ", (int)map->entries[i].key->length,
                       map->entries[i].key->data);
            }
            print_value(map->entries[i].value, color);
        }
        if (color) printf("%s}%s", CLI_CYAN, CLI_RESET);
        else printf("}");
    } else if (IS_PTR(value)) {
        // Other object types (functions, closures, etc.)
        ObjHeader* obj = (ObjHeader*)AS_PTR(value);
        switch (obj->type) {
            case OBJ_FUNCTION: {
                ObjFunction* fn = (ObjFunction*)obj;
                if (fn->name) {
                    if (color) printf("%s<fn %.*s>%s", CLI_CYAN,
                                      (int)fn->name->length, fn->name->data, CLI_RESET);
                    else printf("<fn %.*s>", (int)fn->name->length, fn->name->data);
                } else {
                    if (color) printf("%s<fn>%s", CLI_CYAN, CLI_RESET);
                    else printf("<fn>");
                }
                break;
            }
            case OBJ_CLOSURE: {
                ObjClosure* cl = (ObjClosure*)obj;
                if (cl->function && cl->function->name) {
                    if (color) printf("%s<fn %.*s>%s", CLI_CYAN,
                                      (int)cl->function->name->length,
                                      cl->function->name->data, CLI_RESET);
                    else printf("<fn %.*s>", (int)cl->function->name->length,
                               cl->function->name->data);
                } else {
                    if (color) printf("%s<fn>%s", CLI_CYAN, CLI_RESET);
                    else printf("<fn>");
                }
                break;
            }
            case OBJ_NATIVE: {
                ObjNative* native = (ObjNative*)obj;
                if (color) printf("%s<native %s>%s", CLI_CYAN,
                                  native->name ? native->name : "?", CLI_RESET);
                else printf("<native %s>", native->name ? native->name : "?");
                break;
            }
            default:
                if (color) printf("%s<object>%s", CLI_CYAN, CLI_RESET);
                else printf("<object>");
                break;
        }
    }
}

// ---- REPL core ----

static void print_prompt(bool continuation, bool color) {
    if (continuation) {
        if (color) printf("%s...> %s", CLI_DIM, CLI_RESET);
        else printf("...> ");
    } else {
        if (color) printf("%svek> %s", CLI_GREEN, CLI_RESET);
        else printf("vek> ");
    }
    fflush(stdout);
}

int cmd_repl_run(int argc, char** argv) {
    if (cli_has_flag(argc, argv, "--help")) {
        printf("Usage: vek repl\n");
        printf("\n");
        printf("Start an enhanced interactive REPL.\n");
        printf("\n");
        printf("Features:\n");
        printf("  - Multi-line input (blocks auto-detected)\n");
        printf("  - Expression result printing\n");
        printf("  - Type 'exit' or press Ctrl-D to quit\n");
        return 0;
    }

    bool color = cli_color_enabled();

    // Initialize VM
    cli_set_args(argc, argv);
    gc_init();
    intern_table_init();
    heap_init();
    vm_init();
    stdlib_init();

    // Print banner
    if (color) {
        printf("%s%svek%s %s REPL (type 'exit' or Ctrl-D to quit)\n",
               CLI_BOLD, CLI_CYAN, CLI_RESET, VEK_VERSION_STRING);
    } else {
        printf("vek %s REPL (type 'exit' or Ctrl-D to quit)\n", VEK_VERSION_STRING);
    }

    char line[REPL_LINE_MAX];
    char input[REPL_INPUT_MAX];
    input[0] = '\0';
    size_t input_len = 0;
    bool in_multiline = false;

    for (;;) {
        print_prompt(in_multiline, color);

        if (!fgets(line, sizeof(line), stdin)) {
            // EOF (Ctrl-D)
            printf("\n");
            break;
        }

        // Check for exit command (only at start of new input)
        if (!in_multiline) {
            // Strip leading whitespace for exit check
            const char* trimmed = line;
            while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

            // Check "exit" followed by newline, null, or whitespace
            if (strncmp(trimmed, "exit", 4) == 0 &&
                (trimmed[4] == '\n' || trimmed[4] == '\0' ||
                 trimmed[4] == ' ' || trimmed[4] == '\t')) {
                break;
            }
        }

        // Append line to input buffer
        size_t line_len = strlen(line);
        if (input_len + line_len >= REPL_INPUT_MAX - 1) {
            fprintf(stderr, "Error: input too long, discarding.\n");
            input[0] = '\0';
            input_len = 0;
            in_multiline = false;
            continue;
        }
        memcpy(input + input_len, line, line_len);
        input_len += line_len;
        input[input_len] = '\0';

        // Check if input is complete
        if (input_is_incomplete(input)) {
            in_multiline = true;
            continue;
        }

        // Input is complete - execute it
        in_multiline = false;

        // Skip empty input
        bool all_whitespace = true;
        for (size_t i = 0; i < input_len; i++) {
            if (input[i] != ' ' && input[i] != '\t' &&
                input[i] != '\n' && input[i] != '\r') {
                all_whitespace = false;
                break;
            }
        }
        if (all_whitespace) {
            input[0] = '\0';
            input_len = 0;
            continue;
        }

        // Save to history
        repl_history_add(input);

        // Record stack top before execution
        Value* stack_before = vm.stack_top;

        // Execute
        InterpretResult result = vm_interpret(input);

        // Print result if expression left a value on stack
        if (result == INTERPRET_OK && vm.stack_top > stack_before) {
            Value top = vm_peek(0);
            if (!IS_NIL(top)) {
                printf("=> ");
                print_value(top, color);
                printf("\n");
            }
            // Pop the result to keep stack clean
            vm_pop();
        }

        // Reset input buffer
        input[0] = '\0';
        input_len = 0;
    }

    repl_history_free();

    vm_free();
    intern_table_destroy();
    gc_destroy();
    heap_destroy();

    return 0;
}
