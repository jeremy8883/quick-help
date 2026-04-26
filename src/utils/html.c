#include "html.h"
#include <string.h>

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

            if (!in_script && !in_style &&
                (g_ascii_strncasecmp(name, "br", 2) == 0 ||
                 g_ascii_strncasecmp(name, "p",  1) == 0 ||
                 g_ascii_strncasecmp(name, "div",3) == 0 ||
                 g_ascii_strncasecmp(name, "li", 2) == 0 ||
                 g_ascii_strncasecmp(name, "tr", 2) == 0 ||
                 g_ascii_strncasecmp(name, "h1", 2) == 0 ||
                 g_ascii_strncasecmp(name, "h2", 2) == 0 ||
                 g_ascii_strncasecmp(name, "h3", 2) == 0 ||
                 g_ascii_strncasecmp(name, "h4", 2) == 0 ||
                 g_ascii_strncasecmp(name, "h5", 2) == 0 ||
                 g_ascii_strncasecmp(name, "h6", 2) == 0)) {
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
