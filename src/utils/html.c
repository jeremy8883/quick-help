#include "html.h"
#include <string.h>

static gboolean is_block_tag(const char *name, const char *gt) {
    static const struct { const char *tag; int len; } block_tags[] = {
        {"br",2}, {"p",1}, {"div",3}, {"li",2}, {"tr",2},
        {"h1",2}, {"h2",2}, {"h3",2}, {"h4",2}, {"h5",2}, {"h6",2},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(block_tags); i++) {
        if (name + block_tags[i].len <= gt &&
            g_ascii_strncasecmp(name, block_tags[i].tag, block_tags[i].len) == 0)
            return TRUE;
    }
    return FALSE;
}

char *html_strip_tags(const char *html, size_t len, size_t max_chars) {
    GString *out = g_string_sized_new(len / 3);
    const char *p = html, *end = html + len;
    gboolean in_script = FALSE, in_style = FALSE;
    int blank_lines = 0;

    while (p < end) {
        if (*p == '<') {
            const char *gt = memchr(p, '>', end - p);
            if (!gt) break;
            const char *tag = p + 1;
            gboolean closing = (*tag == '/');
            const char *name = closing ? tag + 1 : tag;
            while (name < gt && *name == ' ') name++;

            if (g_ascii_strncasecmp(name, "script", 6) == 0)
                in_script = !closing;
            else if (g_ascii_strncasecmp(name, "style", 5) == 0)
                in_style = !closing;

            if (!in_script && !in_style && is_block_tag(name, gt)) {
                if (blank_lines < 2) {
                    g_string_append_c(out, '\n');
                    blank_lines++;
                }
            }
            p = gt + 1;
        } else if (in_script || in_style) {
            p++;
        } else if (*p == '&') {
            if (g_str_has_prefix(p, "&amp;"))       { g_string_append_c(out, '&');  p += 5; }
            else if (g_str_has_prefix(p, "&lt;"))    { g_string_append_c(out, '<');  p += 4; }
            else if (g_str_has_prefix(p, "&gt;"))    { g_string_append_c(out, '>');  p += 4; }
            else if (g_str_has_prefix(p, "&quot;"))  { g_string_append_c(out, '"');  p += 6; }
            else if (g_str_has_prefix(p, "&nbsp;"))  { g_string_append_c(out, ' ');  p += 6; }
            else if (g_str_has_prefix(p, "&#")) {
                const char *semi = memchr(p, ';', MIN((size_t)(end - p), 10));
                p = semi ? semi + 1 : p + 1;
            } else { g_string_append_c(out, '&'); p++; }
            blank_lines = 0;
        } else if (*p == '\n' || *p == '\r') {
            if (blank_lines < 2) { g_string_append_c(out, '\n'); blank_lines++; }
            p++;
        } else if (*p == ' ' || *p == '\t') {
            if (out->len > 0 && out->str[out->len-1] != ' ' && out->str[out->len-1] != '\n')
                g_string_append_c(out, ' ');
            p++;
        } else {
            g_string_append_c(out, *p);
            blank_lines = 0;
            p++;
        }
        if (out->len > max_chars) {
            g_string_append(out, "\n[content truncated]");
            break;
        }
    }
    return g_string_free(out, FALSE);
}
