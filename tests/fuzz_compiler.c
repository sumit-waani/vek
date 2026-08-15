/*
 * Fuzz harness for the vek compiler pipeline using libFuzzer.
 * Feeds arbitrary input through the full compile pipeline (lexer -> compiler).
 * Catches crashes in the parser/compiler on arbitrary input.
 *
 * Build: make fuzz_compiler
 * Run:   ./build/fuzz_compiler (optionally with a corpus directory)
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "lexer.h"
#include "compiler.h"
#include "vm.h"
#include "gc.h"
#include "memory.h"
#include "object.h"

// libFuzzer entry point
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // Null-terminate the input
    char *input = malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';

    // Initialize VM subsystems
    gc_init();
    intern_table_init();
    heap_init();
    vm_init();

    // Try to compile (may fail gracefully on invalid input)
    ObjFunction *fn = compile(input);
    (void)fn; // We don't need to execute, just compile

    // Clean up
    vm_free();
    intern_table_destroy();
    gc_destroy();
    heap_destroy();

    free(input);
    return 0;
}
