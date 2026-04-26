#include "markdown.h"
#include <glib.h>
#include <string.h>

/* Apply inline markdown formatting (bold, code) to an already-escaped line */
static void append_inline_formatted(GString *out, const char *escaped) {
    const char *s = escaped;
    while (*s) {
        /* Bold: **text** */
        if (s[0] == '*' && s[1] == '*') {
            const char *end = strstr(s + 2, "**");
            if (end) {
                g_string_append(out, "<b>");
                g_string_append_len(out, s + 2, end - (s + 2));
                g_string_append(out, "</b>");
                s = end + 2;
                continue;
            }
        }
        /* Inline code: `text` */
        if (s[0] == '`') {
            const char *end = strchr(s + 1, '`');
            if (end) {
                g_string_append(out, "<tt>");
                g_string_append_len(out, s + 1, end - (s + 1));
                g_string_append(out, "</tt>");
                s = end + 1;
                continue;
            }
        }
        g_string_append_c(out, *s);
        s++;
    }
}

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
            in_code_block = !in_code_block;
            g_string_append(out, in_code_block ? "<tt>" : "</tt>");
        } else if (in_code_block) {
            /* Inside code block: just escape and append as monospace */
            char *escaped = g_markup_escape_text(line, -1);
            g_string_append(out, escaped);
            g_string_append_c(out, '\n');
            g_free(escaped);
        } else if (line[0] == '#') {
            /* Heading: # text */
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
        } else {
            /* Check for list item: - text or * text */
            const char *trimmed = line;
            while (*trimmed == ' ') trimmed++;
            if ((*trimmed == '-' || *trimmed == '*') && trimmed[1] == ' ') {
                int indent = trimmed - line;
                char *escaped = g_markup_escape_text(trimmed + 2, -1);
                for (int i = 0; i < indent; i++)
                    g_string_append_c(out, ' ');
                g_string_append_printf(out, " \xe2\x80\xa2 %s\n", escaped);
                g_free(escaped);
            } else {
                /* Regular line: escape, then apply inline formatting */
                char *escaped = g_markup_escape_text(line, -1);
                append_inline_formatted(out, escaped);
                g_string_append_c(out, '\n');
                g_free(escaped);
            }
        }

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
