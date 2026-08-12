#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ---- Builder: growable byte buffer ----

#define BUILDER_INITIAL_CAP 4096

typedef struct {
    char*  buf;
    size_t len;
    size_t cap;
} Builder;

// Builder stack (for nested render_inline calls)
// NOTE: This is process-global mutable state. The current design assumes
// single-request-at-a-time processing. Concurrent or pipelined request
// handling would require per-request context objects instead.
#define BUILDER_STACK_MAX 32

static Builder builder_stack[BUILDER_STACK_MAX];
static int builder_stack_count = 0;

static void builder_init(Builder* b) {
    b->cap = BUILDER_INITIAL_CAP;
    b->len = 0;
    b->buf = (char*)malloc(b->cap);
    b->buf[0] = '\0';
}

static void builder_free(Builder* b) {
    free(b->buf);
    b->buf = NULL;
    b->len = 0;
    b->cap = 0;
}

static void builder_write(Builder* b, const char* str, size_t slen) {
    while (b->len + slen + 1 > b->cap) {
        b->cap = b->cap * 2;
        b->buf = (char*)realloc(b->buf, b->cap);
    }
    memcpy(b->buf + b->len, str, slen);
    b->len += slen;
    b->buf[b->len] = '\0';
}

static void builder_write_str(Builder* b, const char* str) {
    builder_write(b, str, strlen(str));
}

static void builder_write_char(Builder* b, char c) {
    builder_write(b, &c, 1);
}

// Write HTML-escaped content (escapes & < > " ')
static void builder_write_escaped(Builder* b, const char* str, size_t slen) {
    for (size_t i = 0; i < slen; i++) {
        char c = str[i];
        switch (c) {
            case '&':  builder_write(b, "&amp;", 5); break;
            case '<':  builder_write(b, "&lt;", 4); break;
            case '>':  builder_write(b, "&gt;", 4); break;
            case '"':  builder_write(b, "&quot;", 6); break;
            case '\'': builder_write(b, "&#39;", 5); break;
            default:   builder_write_char(b, c); break;
        }
    }
}

// Finish builder and return an ObjString
static ObjString* builder_finish(Builder* b) {
    ObjString* result = obj_string_new(b->buf, (uint32_t)b->len);
    return result;
}

// Get the current active builder (top of stack), or NULL if none
static Builder* builder_current(void) {
    if (builder_stack_count <= 0) return NULL;
    return &builder_stack[builder_stack_count - 1];
}

// Push a new builder onto the stack
static Builder* builder_push(void) {
    if (builder_stack_count >= BUILDER_STACK_MAX) return NULL;
    Builder* b = &builder_stack[builder_stack_count++];
    builder_init(b);
    return b;
}

// Pop the current builder (caller must free if needed)
static Builder* builder_pop(void) {
    if (builder_stack_count <= 0) return NULL;
    builder_stack_count--;
    return &builder_stack[builder_stack_count];
}

// ---- Void elements (self-closing) ----

static bool is_void_element(const char* tag) {
    static const char* void_tags[] = {
        "br", "hr", "img", "input", "meta", "link",
        "area", "base", "col", "embed", "source", "track", "wbr",
        NULL
    };
    for (int i = 0; void_tags[i] != NULL; i++) {
        if (strcmp(tag, void_tags[i]) == 0) return true;
    }
    return false;
}

// ---- Write attributes from an ObjMap ----

static void builder_write_attrs(Builder* b, ObjMap* map) {
    for (uint32_t i = 0; i < map->capacity; i++) {
        if (map->entries[i].key == NULL) continue;
        if (map->entries[i].key == MAP_TOMBSTONE) continue;

        ObjString* key = map->entries[i].key;
        Value val = map->entries[i].value;

        builder_write_char(b, ' ');
        builder_write(b, key->data, key->length);
        builder_write_str(b, "=\"");

        if (IS_STRING(val)) {
            ObjString* sval = AS_STRING(val);
            builder_write_escaped(b, sval->data, sval->length);
        } else if (IS_INT(val)) {
            char num[32];
            int nlen = snprintf(num, sizeof(num), "%lld", (long long)AS_INT(val));
            builder_write(b, num, (size_t)nlen);
        } else if (IS_FLOAT(val)) {
            char num[64];
            int nlen = snprintf(num, sizeof(num), "%g", AS_DOUBLE(val));
            builder_write(b, num, (size_t)nlen);
        } else if (IS_BOOL(val)) {
            if (AS_BOOL(val)) {
                builder_write_str(b, "true");
            } else {
                builder_write_str(b, "false");
            }
        }

        builder_write_char(b, '"');
    }
}

// ---- Generic tag helper ----
// Handles all the tag generation logic.
// tag_name: the HTML tag name (e.g., "div", "h1")
// argc: number of arguments passed
// args: the argument values
// Returns VAL_NIL (output goes to the builder)

static Value html_tag_helper(const char* tag_name, int argc, Value* args) {
    Builder* b = builder_current();
    if (!b) {
        // No active builder - push a temporary one if needed
        // This should not happen in normal usage (render_inline sets up a builder)
        return VAL_NIL;
    }

    bool is_void = is_void_element(tag_name);
    ObjMap* attrs = NULL;
    Value content = VAL_NIL;
    Value closure = VAL_NIL;

    // Parse arguments:
    // 0 args: <tag></tag> or <tag> for void
    // 1 arg:  string content, map attrs, or closure
    // 2 args: map attrs + string content or closure

    if (argc >= 1) {
        if (IS_MAP(args[0])) {
            attrs = AS_MAP(args[0]);
            if (argc >= 2) {
                if (IS_CLOSURE(args[1]) || IS_NATIVE(args[1])) {
                    closure = args[1];
                } else if (IS_STRING(args[1])) {
                    content = args[1];
                }
            }
        } else if (IS_CLOSURE(args[0]) || IS_NATIVE(args[0])) {
            closure = args[0];
        } else if (IS_STRING(args[0])) {
            content = args[0];
        }
    }

    // Write opening tag
    builder_write_char(b, '<');
    builder_write_str(b, tag_name);

    if (attrs) {
        builder_write_attrs(b, attrs);
    }

    if (is_void) {
        builder_write_char(b, '>');
        return VAL_NIL;
    }

    builder_write_char(b, '>');

    // Write content
    if (!IS_NIL(content)) {
        ObjString* s = AS_STRING(content);
        builder_write_escaped(b, s->data, s->length);
    } else if (!IS_NIL(closure)) {
        // Call the closure - it will write to the current builder
        vm_push(closure);
        vm_call(closure, 0);
    }

    // Write closing tag
    builder_write_str(b, "</");
    builder_write_str(b, tag_name);
    builder_write_char(b, '>');

    return VAL_NIL;
}

// ---- Native function implementations ----
// Each tag is a native function with variadic arity (-1)

#define DEFINE_TAG_FN(fname, tagname) \
    static Value native_html_##fname(int argc, Value* args) { \
        return html_tag_helper(tagname, argc, args); \
    }

DEFINE_TAG_FN(html, "html")
DEFINE_TAG_FN(head, "head")
DEFINE_TAG_FN(body, "body")
DEFINE_TAG_FN(div, "div")
DEFINE_TAG_FN(span, "span")
DEFINE_TAG_FN(p, "p")
DEFINE_TAG_FN(h1, "h1")
DEFINE_TAG_FN(h2, "h2")
DEFINE_TAG_FN(h3, "h3")
DEFINE_TAG_FN(h4, "h4")
DEFINE_TAG_FN(h5, "h5")
DEFINE_TAG_FN(h6, "h6")
DEFINE_TAG_FN(a, "a")
DEFINE_TAG_FN(ul, "ul")
DEFINE_TAG_FN(ol, "ol")
DEFINE_TAG_FN(li, "li")
DEFINE_TAG_FN(form, "form")
DEFINE_TAG_FN(input, "input")
DEFINE_TAG_FN(button, "button")
DEFINE_TAG_FN(label, "label")
DEFINE_TAG_FN(textarea, "textarea")
DEFINE_TAG_FN(select_el, "select")
DEFINE_TAG_FN(option, "option")
DEFINE_TAG_FN(table, "table")
DEFINE_TAG_FN(tr, "tr")
DEFINE_TAG_FN(td, "td")
DEFINE_TAG_FN(th, "th")
DEFINE_TAG_FN(header, "header")
DEFINE_TAG_FN(footer, "footer")
DEFINE_TAG_FN(main_el, "main")
DEFINE_TAG_FN(nav, "nav")
DEFINE_TAG_FN(section, "section")
DEFINE_TAG_FN(article, "article")
DEFINE_TAG_FN(img, "img")
DEFINE_TAG_FN(br, "br")
DEFINE_TAG_FN(hr, "hr")
DEFINE_TAG_FN(meta, "meta")
DEFINE_TAG_FN(link, "link")
DEFINE_TAG_FN(title, "title")
DEFINE_TAG_FN(script, "script")
DEFINE_TAG_FN(style, "style")
DEFINE_TAG_FN(small, "small")
DEFINE_TAG_FN(strong, "strong")
DEFINE_TAG_FN(em, "em")
DEFINE_TAG_FN(pre, "pre")
DEFINE_TAG_FN(code, "code")

// ---- raw(string) - write unescaped content ----

static Value native_html_raw(int argc, Value* args) {
    (void)argc;
    Builder* b = builder_current();
    if (!b) return VAL_NIL;

    if (argc >= 1 && IS_STRING(args[0])) {
        ObjString* s = AS_STRING(args[0]);
        builder_write(b, s->data, s->length);
    }
    return VAL_NIL;
}

// ---- doctype() - write <!DOCTYPE html> ----

static Value native_html_doctype(int argc, Value* args) {
    (void)argc;
    (void)args;
    Builder* b = builder_current();
    if (!b) return VAL_NIL;

    builder_write_str(b, "<!DOCTYPE html>");
    return VAL_NIL;
}

// ---- render_inline(closure) - render to string ----

static Value native_html_render_inline(int argc, Value* args) {
    (void)argc;
    if (argc < 1) return VAL_NIL;

    Value closure = args[0];
    if (!IS_CLOSURE(closure) && !IS_NATIVE(closure)) {
        return VAL_NIL;
    }

    // Push a new builder
    Builder* b = builder_push();
    if (!b) return VAL_NIL;

    // Call the closure
    vm_push(closure);
    vm_call(closure, 0);

    // Pop builder and get result
    Builder* finished = builder_pop();
    ObjString* result = builder_finish(finished);
    gc_push_root(OBJ_VAL(result));
    builder_free(finished);
    gc_pop_root();

    return OBJ_VAL(result);
}

// ---- text(string) - write escaped text content directly ----

static Value native_html_text(int argc, Value* args) {
    (void)argc;
    Builder* b = builder_current();
    if (!b) return VAL_NIL;

    if (argc >= 1 && IS_STRING(args[0])) {
        ObjString* s = AS_STRING(args[0]);
        builder_write_escaped(b, s->data, s->length);
    }
    return VAL_NIL;
}

// ---- Package registration ----

void stdlib_view_init(ObjMap* pkg) {
    // HTML tag helpers (all variadic)
    stdlib_register(pkg, "html", native_html_html, -1);
    stdlib_register(pkg, "head", native_html_head, -1);
    stdlib_register(pkg, "body", native_html_body, -1);
    stdlib_register(pkg, "div", native_html_div, -1);
    stdlib_register(pkg, "span", native_html_span, -1);
    stdlib_register(pkg, "p", native_html_p, -1);
    stdlib_register(pkg, "h1", native_html_h1, -1);
    stdlib_register(pkg, "h2", native_html_h2, -1);
    stdlib_register(pkg, "h3", native_html_h3, -1);
    stdlib_register(pkg, "h4", native_html_h4, -1);
    stdlib_register(pkg, "h5", native_html_h5, -1);
    stdlib_register(pkg, "h6", native_html_h6, -1);
    stdlib_register(pkg, "a", native_html_a, -1);
    stdlib_register(pkg, "ul", native_html_ul, -1);
    stdlib_register(pkg, "ol", native_html_ol, -1);
    stdlib_register(pkg, "li", native_html_li, -1);
    stdlib_register(pkg, "form", native_html_form, -1);
    stdlib_register(pkg, "input", native_html_input, -1);
    stdlib_register(pkg, "button", native_html_button, -1);
    stdlib_register(pkg, "label", native_html_label, -1);
    stdlib_register(pkg, "textarea", native_html_textarea, -1);
    stdlib_register(pkg, "select", native_html_select_el, -1);
    stdlib_register(pkg, "option", native_html_option, -1);
    stdlib_register(pkg, "table", native_html_table, -1);
    stdlib_register(pkg, "tr", native_html_tr, -1);
    stdlib_register(pkg, "td", native_html_td, -1);
    stdlib_register(pkg, "th", native_html_th, -1);
    stdlib_register(pkg, "header", native_html_header, -1);
    stdlib_register(pkg, "footer", native_html_footer, -1);
    stdlib_register(pkg, "main", native_html_main_el, -1);
    stdlib_register(pkg, "nav", native_html_nav, -1);
    stdlib_register(pkg, "section", native_html_section, -1);
    stdlib_register(pkg, "article", native_html_article, -1);
    stdlib_register(pkg, "img", native_html_img, -1);
    stdlib_register(pkg, "br", native_html_br, -1);
    stdlib_register(pkg, "hr", native_html_hr, -1);
    stdlib_register(pkg, "meta", native_html_meta, -1);
    stdlib_register(pkg, "link", native_html_link, -1);
    stdlib_register(pkg, "title", native_html_title, -1);
    stdlib_register(pkg, "script", native_html_script, -1);
    stdlib_register(pkg, "style", native_html_style, -1);
    stdlib_register(pkg, "small", native_html_small, -1);
    stdlib_register(pkg, "strong", native_html_strong, -1);
    stdlib_register(pkg, "em", native_html_em, -1);
    stdlib_register(pkg, "pre", native_html_pre, -1);
    stdlib_register(pkg, "code", native_html_code, -1);

    // Utility functions
    stdlib_register(pkg, "raw", native_html_raw, -1);
    stdlib_register(pkg, "text", native_html_text, -1);
    stdlib_register(pkg, "doctype", native_html_doctype, -1);
    stdlib_register(pkg, "render_inline", native_html_render_inline, 1);
}
