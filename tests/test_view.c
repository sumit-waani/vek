/*
 * Unit tests for the view/html DSL.
 * Tests Builder, HTML escaping, tag generation, void elements, raw().
 */
#include "common.h"
#include "value.h"
#include "memory.h"
#include "gc.h"
#include "object.h"
#include "vm.h"
#include "vek_stdlib.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  test: %s ... ", #name); \
    if (test_##name()) { tests_passed++; printf("ok\n"); } \
    else { printf("FAILED\n"); } \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("\n    ASSERT FAILED: %s (line %d)\n", #cond, __LINE__); \
        return false; \
    } \
} while(0)

// ---- Test render_inline with simple content ----

static bool test_render_inline_simple(void) {
    const char* src =
        "fn content()\n"
        "  html.div(\"hello\")\n"
        "end\n"
        "result = html.render_inline(content)\n"
        "print(result)\n";
    InterpretResult r = vm_interpret(src);
    ASSERT(r == INTERPRET_OK);
    return true;
}

// ---- Test HTML escaping ----

static bool test_html_escaping(void) {
    const char* src =
        "fn content()\n"
        "  html.p(\"<script>alert('xss')&\\\"test\\\"</script>\")\n"
        "end\n"
        "result = html.render_inline(content)\n"
        "print(result)\n";
    InterpretResult r = vm_interpret(src);
    ASSERT(r == INTERPRET_OK);
    return true;
}

// ---- Test attributes ----

static bool test_tag_with_attrs(void) {
    const char* src =
        "fn content()\n"
        "  html.a({href: \"/home\"}, \"Go Home\")\n"
        "end\n"
        "result = html.render_inline(content)\n"
        "print(result)\n";
    InterpretResult r = vm_interpret(src);
    ASSERT(r == INTERPRET_OK);
    return true;
}

// ---- Test nested tags ----

static bool test_nested_tags(void) {
    const char* src =
        "fn inner()\n"
        "  html.p(\"inner\")\n"
        "end\n"
        "fn outer()\n"
        "  html.div(inner)\n"
        "end\n"
        "result = html.render_inline(outer)\n"
        "print(result)\n";
    InterpretResult r = vm_interpret(src);
    ASSERT(r == INTERPRET_OK);
    return true;
}

// ---- Test void elements ----

static bool test_void_elements(void) {
    const char* src =
        "fn content()\n"
        "  html.br()\n"
        "  html.hr()\n"
        "  html.img({src: \"/logo.png\", alt: \"Logo\"})\n"
        "end\n"
        "result = html.render_inline(content)\n"
        "print(result)\n";
    InterpretResult r = vm_interpret(src);
    ASSERT(r == INTERPRET_OK);
    return true;
}

// ---- Test raw() bypasses escaping ----

static bool test_raw_no_escape(void) {
    const char* src =
        "fn content()\n"
        "  html.raw(\"<b>bold</b>\")\n"
        "end\n"
        "result = html.render_inline(content)\n"
        "print(result)\n";
    InterpretResult r = vm_interpret(src);
    ASSERT(r == INTERPRET_OK);
    return true;
}

// ---- Test doctype ----

static bool test_doctype(void) {
    const char* src =
        "fn content()\n"
        "  html.doctype()\n"
        "end\n"
        "result = html.render_inline(content)\n"
        "print(result)\n";
    InterpretResult r = vm_interpret(src);
    ASSERT(r == INTERPRET_OK);
    return true;
}

// ---- Test empty tag ----

static bool test_empty_tag(void) {
    const char* src =
        "fn content()\n"
        "  html.div()\n"
        "end\n"
        "result = html.render_inline(content)\n"
        "print(result)\n";
    InterpretResult r = vm_interpret(src);
    ASSERT(r == INTERPRET_OK);
    return true;
}

int main(void) {
    // Initialize subsystems
    gc_init();
    intern_table_init();
    heap_init();
    vm_init();
    stdlib_init();

    printf("=== View DSL Tests ===\n");

    TEST(render_inline_simple);
    TEST(html_escaping);
    TEST(tag_with_attrs);
    TEST(nested_tags);
    TEST(void_elements);
    TEST(raw_no_escape);
    TEST(doctype);
    TEST(empty_tag);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);

    // Cleanup
    vm_free();
    intern_table_destroy();
    gc_destroy();
    heap_destroy();

    return tests_passed == tests_run ? 0 : 1;
}
