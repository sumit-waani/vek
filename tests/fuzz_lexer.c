/*
 * Fuzz harness for the vek lexer using libFuzzer.
 * Feeds arbitrary byte input to the lexer and catches crashes/hangs.
 *
 * Build: make fuzz_lexer
 * Run:   ./build/fuzz_lexer (optionally with a corpus directory)
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lexer.h"

// libFuzzer entry point
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // Null-terminate the input
    char *input = malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';

    Lexer lexer;
    lexer_init(&lexer, input);

    Token tok;
    int max_tokens = 100000; // prevent infinite loops
    do {
        tok = lexer_next_token(&lexer);
        max_tokens--;
    } while (tok.type != TOKEN_EOF && max_tokens > 0);

    free(input);
    return 0;
}
