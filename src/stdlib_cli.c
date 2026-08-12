#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

// Global argc/argv storage (set by main before stdlib_init)
static int cli_argc = 0;
static char** cli_argv = NULL;

// Public function called from main.c to store args
void cli_set_args(int argc, char** argv) {
    cli_argc = argc;
    cli_argv = argv;
}

// ---- cli.args() ----
// Returns list of command-line arguments (skips vek executable and "run" command)

static Value native_cli_args(int arg_count, Value* args) {
    (void)arg_count;
    (void)args;

    ObjList* list = obj_list_new();
    gc_push_root(OBJ_VAL(list));

    // Skip "vek run file.ve" - start from argv[3] if available
    int start = 3; // skip: vek, run, file.ve
    for (int i = start; i < cli_argc; i++) {
        size_t len = strlen(cli_argv[i]);
        ObjString* s = obj_string_new(cli_argv[i], (uint32_t)len);
        obj_list_push(list, OBJ_VAL(s));
    }

    gc_pop_root();
    return OBJ_VAL(list);
}

// ---- cli.read_line() ----
// Read a line from stdin, returns string (without newline) or nil on EOF

static Value native_cli_read_line(int arg_count, Value* args) {
    (void)arg_count;
    (void)args;

    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) {
        return VAL_NIL;
    }

    // Strip trailing newline
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        len--;
    }
    if (len > 0 && buf[len - 1] == '\r') {
        len--;
    }

    ObjString* result = obj_string_new(buf, (uint32_t)len);
    return OBJ_VAL(result);
}

// ---- cli.print_color(text, color) ----
// Prints text with ANSI color codes

static Value native_cli_print_color(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0]) || !IS_STRING(args[1])) return VAL_NIL;

    ObjString* text = AS_STRING(args[0]);
    ObjString* color = AS_STRING(args[1]);

    const char* code = NULL;
    if (color->length == 3 && memcmp(color->data, "red", 3) == 0) code = "\033[31m";
    else if (color->length == 5 && memcmp(color->data, "green", 5) == 0) code = "\033[32m";
    else if (color->length == 6 && memcmp(color->data, "yellow", 6) == 0) code = "\033[33m";
    else if (color->length == 4 && memcmp(color->data, "blue", 4) == 0) code = "\033[34m";
    else if (color->length == 7 && memcmp(color->data, "magenta", 7) == 0) code = "\033[35m";
    else if (color->length == 4 && memcmp(color->data, "cyan", 4) == 0) code = "\033[36m";
    else if (color->length == 4 && memcmp(color->data, "bold", 4) == 0) code = "\033[1m";
    else code = "";

    printf("%s%.*s\033[0m", code, (int)text->length, text->data);
    fflush(stdout);

    return VAL_NIL;
}

// ---- cli.prompt(message) ----
// Print message, then read a line from stdin

static Value native_cli_prompt(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;

    ObjString* message = AS_STRING(args[0]);
    printf("%.*s", (int)message->length, message->data);
    fflush(stdout);

    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) {
        return VAL_NIL;
    }

    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') len--;
    if (len > 0 && buf[len - 1] == '\r') len--;

    ObjString* result = obj_string_new(buf, (uint32_t)len);
    return OBJ_VAL(result);
}

void stdlib_cli_init(ObjMap* pkg) {
    stdlib_register(pkg, "args", native_cli_args, 0);
    stdlib_register(pkg, "read_line", native_cli_read_line, 0);
    stdlib_register(pkg, "print_color", native_cli_print_color, 2);
    stdlib_register(pkg, "prompt", native_cli_prompt, 1);
}
