#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

// ---- Dynamic buffer helpers ----

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} MdBuf;

static void md_buf_init(MdBuf* b) {
    b->cap = 256;
    b->data = (char*)malloc(b->cap);
    b->len = 0;
}

static void md_buf_ensure(MdBuf* b, size_t extra) {
    while (b->len + extra >= b->cap) {
        b->cap *= 2;
        b->data = (char*)realloc(b->data, b->cap);
    }
}

static void md_buf_append(MdBuf* b, const char* s, size_t n) {
    md_buf_ensure(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
}

static void md_buf_str(MdBuf* b, const char* s) {
    md_buf_append(b, s, strlen(s));
}

static void md_buf_char(MdBuf* b, char c) {
    md_buf_ensure(b, 1);
    b->data[b->len++] = c;
}

// ---- HTML escaping ----

static void md_escape_append(MdBuf* b, const char* text, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        switch (c) {
            case '&': md_buf_str(b, "&amp;"); break;
            case '<': md_buf_str(b, "&lt;"); break;
            case '>': md_buf_str(b, "&gt;"); break;
            case '"': md_buf_str(b, "&quot;"); break;
            case '\'': md_buf_str(b, "&#x27;"); break;
            default: md_buf_char(b, c); break;
        }
    }
}

// ---- Inline formatting ----

static void md_render_inline(MdBuf* b, const char* text, size_t len) {
    size_t i = 0;
    while (i < len) {
        // Bold: **text**
        if (i + 1 < len && text[i] == '*' && text[i + 1] == '*') {
            size_t start = i + 2;
            size_t end = start;
            while (end + 1 < len && !(text[end] == '*' && text[end + 1] == '*')) {
                end++;
            }
            if (end + 1 < len) {
                md_buf_str(b, "<strong>");
                md_escape_append(b, text + start, end - start);
                md_buf_str(b, "</strong>");
                i = end + 2;
                continue;
            }
        }

        // Italic: *text*
        if (text[i] == '*' && (i + 1 < len) && text[i + 1] != '*') {
            size_t start = i + 1;
            size_t end = start;
            while (end < len && text[end] != '*') {
                end++;
            }
            if (end < len) {
                md_buf_str(b, "<em>");
                md_escape_append(b, text + start, end - start);
                md_buf_str(b, "</em>");
                i = end + 1;
                continue;
            }
        }

        // Inline code: `code`
        if (text[i] == '`') {
            size_t start = i + 1;
            size_t end = start;
            while (end < len && text[end] != '`') {
                end++;
            }
            if (end < len) {
                md_buf_str(b, "<code>");
                md_escape_append(b, text + start, end - start);
                md_buf_str(b, "</code>");
                i = end + 1;
                continue;
            }
        }

        // Links: [text](url)
        if (text[i] == '[') {
            size_t text_start = i + 1;
            size_t text_end = text_start;
            while (text_end < len && text[text_end] != ']') {
                text_end++;
            }
            if (text_end < len && text_end + 1 < len && text[text_end + 1] == '(') {
                size_t url_start = text_end + 2;
                size_t url_end = url_start;
                while (url_end < len && text[url_end] != ')') {
                    url_end++;
                }
                if (url_end < len) {
                    md_buf_str(b, "<a href=\"");
                    md_escape_append(b, text + url_start, url_end - url_start);
                    md_buf_str(b, "\">");
                    md_escape_append(b, text + text_start, text_end - text_start);
                    md_buf_str(b, "</a>");
                    i = url_end + 1;
                    continue;
                }
            }
        }

        // Regular character - escape it
        switch (text[i]) {
            case '&': md_buf_str(b, "&amp;"); break;
            case '<': md_buf_str(b, "&lt;"); break;
            case '>': md_buf_str(b, "&gt;"); break;
            case '"': md_buf_str(b, "&quot;"); break;
            case '\'': md_buf_str(b, "&#x27;"); break;
            default: md_buf_char(b, text[i]); break;
        }
        i++;
    }
}

// ---- Check for horizontal rule ----

static bool is_hr_line(const char* line, size_t len) {
    if (len < 3) return false;
    char c = 0;
    size_t count = 0;
    for (size_t i = 0; i < len; i++) {
        if (line[i] == ' ') continue;
        if (line[i] == '-' || line[i] == '*' || line[i] == '_') {
            if (c == 0) c = line[i];
            else if (line[i] != c) return false;
            count++;
        } else {
            return false;
        }
    }
    return count >= 3;
}

// ---- Main render function ----

static Value native_markdown_render(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;
    ObjString* input = AS_STRING(args[0]);

    MdBuf buf;
    md_buf_init(&buf);

    const char* src = input->data;
    size_t src_len = input->length;

    // Process line by line
    size_t pos = 0;
    bool in_code_block = false;
    bool in_ul = false;
    bool in_ol = false;
    bool in_blockquote = false;
    bool first_block = true;

    while (pos < src_len) {
        // Find end of line
        size_t line_start = pos;
        while (pos < src_len && src[pos] != '\n') pos++;
        size_t line_len = pos - line_start;
        if (pos < src_len) pos++; // skip newline

        const char* line = src + line_start;

        // Code blocks (```)
        if (line_len >= 3 && line[0] == '`' && line[1] == '`' && line[2] == '`') {
            if (!in_code_block) {
                // Close any open lists
                if (in_ul) { md_buf_str(&buf, "</ul>"); in_ul = false; }
                if (in_ol) { md_buf_str(&buf, "</ol>"); in_ol = false; }
                if (in_blockquote) { md_buf_str(&buf, "</blockquote>"); in_blockquote = false; }
                if (!first_block) md_buf_char(&buf, '\n');
                first_block = false;
                md_buf_str(&buf, "<pre><code>");
                in_code_block = true;
            } else {
                md_buf_str(&buf, "</code></pre>");
                in_code_block = false;
            }
            continue;
        }

        if (in_code_block) {
            md_escape_append(&buf, line, line_len);
            md_buf_char(&buf, '\n');
            continue;
        }

        // Empty line - close open blocks
        if (line_len == 0) {
            if (in_ul) { md_buf_str(&buf, "</ul>"); in_ul = false; }
            if (in_ol) { md_buf_str(&buf, "</ol>"); in_ol = false; }
            if (in_blockquote) { md_buf_str(&buf, "</blockquote>"); in_blockquote = false; }
            continue;
        }

        // Horizontal rule
        if (is_hr_line(line, line_len)) {
            if (in_ul) { md_buf_str(&buf, "</ul>"); in_ul = false; }
            if (in_ol) { md_buf_str(&buf, "</ol>"); in_ol = false; }
            if (in_blockquote) { md_buf_str(&buf, "</blockquote>"); in_blockquote = false; }
            if (!first_block) md_buf_char(&buf, '\n');
            first_block = false;
            md_buf_str(&buf, "<hr>");
            continue;
        }

        // Headings (# to ######)
        if (line[0] == '#') {
            int level = 0;
            while ((size_t)level < line_len && line[level] == '#' && level < 6) level++;
            if ((size_t)level < line_len && line[level] == ' ') {
                if (in_ul) { md_buf_str(&buf, "</ul>"); in_ul = false; }
                if (in_ol) { md_buf_str(&buf, "</ol>"); in_ol = false; }
                if (in_blockquote) { md_buf_str(&buf, "</blockquote>"); in_blockquote = false; }
                if (!first_block) md_buf_char(&buf, '\n');
                first_block = false;
                char tag[8];
                snprintf(tag, sizeof(tag), "<h%d>", level);
                md_buf_str(&buf, tag);
                md_render_inline(&buf, line + level + 1, line_len - level - 1);
                snprintf(tag, sizeof(tag), "</h%d>", level);
                md_buf_str(&buf, tag);
                continue;
            }
        }

        // Unordered list (- item)
        if (line_len >= 2 && line[0] == '-' && line[1] == ' ') {
            if (in_ol) { md_buf_str(&buf, "</ol>"); in_ol = false; }
            if (in_blockquote) { md_buf_str(&buf, "</blockquote>"); in_blockquote = false; }
            if (!in_ul) {
                if (!first_block) md_buf_char(&buf, '\n');
                first_block = false;
                md_buf_str(&buf, "<ul>");
                in_ul = true;
            }
            md_buf_str(&buf, "<li>");
            md_render_inline(&buf, line + 2, line_len - 2);
            md_buf_str(&buf, "</li>");
            continue;
        }

        // Ordered list (1. item, 2. item, etc.)
        if (line[0] >= '1' && line[0] <= '9') {
            size_t j = 0;
            while (j < line_len && line[j] >= '0' && line[j] <= '9') j++;
            if (j < line_len - 1 && line[j] == '.' && line[j + 1] == ' ') {
                if (in_ul) { md_buf_str(&buf, "</ul>"); in_ul = false; }
                if (in_blockquote) { md_buf_str(&buf, "</blockquote>"); in_blockquote = false; }
                if (!in_ol) {
                    if (!first_block) md_buf_char(&buf, '\n');
                    first_block = false;
                    md_buf_str(&buf, "<ol>");
                    in_ol = true;
                }
                md_buf_str(&buf, "<li>");
                md_render_inline(&buf, line + j + 2, line_len - j - 2);
                md_buf_str(&buf, "</li>");
                continue;
            }
        }

        // Blockquote (> text)
        if (line[0] == '>' && line_len >= 2 && line[1] == ' ') {
            if (in_ul) { md_buf_str(&buf, "</ul>"); in_ul = false; }
            if (in_ol) { md_buf_str(&buf, "</ol>"); in_ol = false; }
            if (!in_blockquote) {
                if (!first_block) md_buf_char(&buf, '\n');
                first_block = false;
                md_buf_str(&buf, "<blockquote>");
                in_blockquote = true;
            }
            md_render_inline(&buf, line + 2, line_len - 2);
            continue;
        }

        // Paragraph (default)
        if (in_ul) { md_buf_str(&buf, "</ul>"); in_ul = false; }
        if (in_ol) { md_buf_str(&buf, "</ol>"); in_ol = false; }
        if (in_blockquote) { md_buf_str(&buf, "</blockquote>"); in_blockquote = false; }
        if (!first_block) md_buf_char(&buf, '\n');
        first_block = false;
        md_buf_str(&buf, "<p>");
        md_render_inline(&buf, line, line_len);
        md_buf_str(&buf, "</p>");
    }

    // Close any open blocks
    if (in_code_block) md_buf_str(&buf, "</code></pre>");
    if (in_ul) md_buf_str(&buf, "</ul>");
    if (in_ol) md_buf_str(&buf, "</ol>");
    if (in_blockquote) md_buf_str(&buf, "</blockquote>");

    ObjString* result = obj_string_new(buf.data, (uint32_t)buf.len);
    free(buf.data);
    return OBJ_VAL(result);
}

void stdlib_markdown_init(ObjMap* pkg) {
    stdlib_register(pkg, "render", native_markdown_render, 1);
}
