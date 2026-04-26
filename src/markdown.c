#include "markdown.h"
#include <glib.h>
#include <string.h>

/* Escape text for Pango markup, then apply markdown conversions */
char *markdown_to_pango(const char *md) {
    if (!md || !*md) return g_strdup("");

    GString *out = g_string_new(NULL);
    const char *p = md;
    gboolean in_code_block = FALSE;

    while (*p) {
        /* Find end of current line */
        const char *eol = strchr(p, '\n');
        if (!eol) eol = p + strlen(p);
        gsize line_len = eol - p;
        char *line = g_strndup(p, line_len);

        /* Code block toggle */
        if (g_str_has_prefix(line, "```")) {
            if (!in_code_block) {
                g_string_append(out, "<tt>");
                in_code_block = TRUE;
            } else {
                g_string_append(out, "</tt>");
                in_code_block = FALSE;
            }
            g_free(line);
            p = *eol ? eol + 1 : eol;
            continue;
        }

        if (in_code_block) {
            /* Inside code block: just escape and append as monospace */
            char *escaped = g_markup_escape_text(line, -1);
            g_string_append(out, escaped);
            g_string_append_c(out, '\n');
            g_free(escaped);
            g_free(line);
            p = *eol ? eol + 1 : eol;
            continue;
        }

        /* Heading: # text */
        if (line[0] == '#') {
            const char *text = line;
            int level = 0;
            while (*text == '#') { text++; level++; }
            while (*text == ' ') text++;
            char *escaped = g_markup_escape_text(text, -1);
            if (level == 1)
                g_string_append_printf(out, "<b><big>%s</big></b>", escaped);
            else
                g_string_append_printf(out, "<b>%s</b>", escaped);
            g_string_append_c(out, '\n');
            g_free(escaped);
            g_free(line);
            p = *eol ? eol + 1 : eol;
            continue;
        }

        /* List item: - text or * text */
        const char *trimmed = line;
        while (*trimmed == ' ') trimmed++;
        if ((*trimmed == '-' || *trimmed == '*') && trimmed[1] == ' ') {
            int indent = trimmed - line;
            const char *item_text = trimmed + 2;
            char *escaped = g_markup_escape_text(item_text, -1);
            for (int i = 0; i < indent; i++)
                g_string_append_c(out, ' ');
            g_string_append_printf(out, " \xe2\x80\xa2 %s\n", escaped);
            g_free(escaped);
            g_free(line);
            p = *eol ? eol + 1 : eol;
            continue;
        }

        /* Regular line: escape, then apply inline formatting */
        char *escaped = g_markup_escape_text(line, -1);
        GString *formatted = g_string_new(NULL);
        const char *s = escaped;

        while (*s) {
            /* Bold: **text** */
            if (s[0] == '*' && s[1] == '*') {
                const char *end = strstr(s + 2, "**");
                if (end) {
                    g_string_append(formatted, "<b>");
                    g_string_append_len(formatted, s + 2, end - (s + 2));
                    g_string_append(formatted, "</b>");
                    s = end + 2;
                    continue;
                }
            }
            /* Inline code: `text` */
            if (s[0] == '`') {
                const char *end = strchr(s + 1, '`');
                if (end) {
                    g_string_append(formatted, "<tt>");
                    g_string_append_len(formatted, s + 1, end - (s + 1));
                    g_string_append(formatted, "</tt>");
                    s = end + 1;
                    continue;
                }
            }
            g_string_append_c(formatted, *s);
            s++;
        }

        g_string_append(out, formatted->str);
        g_string_append_c(out, '\n');
        g_string_free(formatted, TRUE);
        g_free(escaped);
        g_free(line);
        p = *eol ? eol + 1 : eol;
    }

    /* Close unclosed code block */
    if (in_code_block)
        g_string_append(out, "</tt>");

    /* Remove trailing newline */
    if (out->len > 0 && out->str[out->len - 1] == '\n')
        g_string_truncate(out, out->len - 1);

    return g_string_free(out, FALSE);
}
