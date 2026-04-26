#include "markdown.h"
#include <glib.h>
#include <string.h>

/* Unescape XML entities in a string segment */
static char *xml_unescape(const char *s, gsize len) {
    GString *out = g_string_sized_new(len);
    for (gsize i = 0; i < len; i++) {
        if (s[i] == '&') {
            if (strncmp(s + i, "&amp;", 5) == 0)  { g_string_append_c(out, '&'); i += 4; }
            else if (strncmp(s + i, "&lt;", 4) == 0)  { g_string_append_c(out, '<'); i += 3; }
            else if (strncmp(s + i, "&gt;", 4) == 0)  { g_string_append_c(out, '>'); i += 3; }
            else if (strncmp(s + i, "&quot;", 6) == 0) { g_string_append_c(out, '"'); i += 5; }
            else if (strncmp(s + i, "&apos;", 6) == 0) { g_string_append_c(out, '\''); i += 5; }
            else g_string_append_c(out, s[i]);
        } else {
            g_string_append_c(out, s[i]);
        }
    }
    return g_string_free(out, FALSE);
}

/* Record a link: collect URL and increment counter */
static void record_link(MarkdownLinkInfo *li, const char *url_escaped, gsize url_len) {
    if (!li) return;
    if (li->urls) {
        char *url = xml_unescape(url_escaped, url_len);
        g_ptr_array_add(li->urls, url);
    }
    li->current_link++;
}

/* Check if the current link should be highlighted */
static gboolean is_highlighted(MarkdownLinkInfo *li) {
    return li && li->highlight_index >= 0 &&
           li->current_link == li->highlight_index;
}

/* Append link markup with optional highlighting */
static void append_link(GString *out, const char *url, gsize url_len,
                        const char *text, gsize text_len,
                        const char *pre_tag, const char *post_tag,
                        MarkdownLinkInfo *li) {
    gboolean hl = is_highlighted(li);
    g_string_append(out, "<a href=\"");
    g_string_append_len(out, url, url_len);
    g_string_append(out, "\">");
    if (hl)
        g_string_append(out, "<span background=\"#3584e4\" foreground=\"white\">");
    if (pre_tag) g_string_append(out, pre_tag);
    g_string_append_len(out, text, text_len);
    if (post_tag) g_string_append(out, post_tag);
    if (hl)
        g_string_append(out, "</span>");
    g_string_append(out, "</a>");
    record_link(li, url, url_len);
}

/* Apply inline markdown formatting (bold, code, links) to an already-escaped line */
static void append_inline_formatted(GString *out, const char *escaped,
                                    MarkdownLinkInfo *li) {
    const char *s = escaped;
    while (*s) {
        /* Markdown link: [text](url) */
        if (s[0] == '[') {
            const char *close_bracket = NULL;
            for (const char *p = s + 1; *p; p++) {
                if (*p == ']' && p[1] == '(') {
                    close_bracket = p;
                    break;
                }
            }
            if (close_bracket) {
                const char *url_start = close_bracket + 2;
                const char *close_paren = strchr(url_start, ')');
                if (close_paren) {
                    append_link(out, url_start, close_paren - url_start,
                               s + 1, close_bracket - (s + 1),
                               NULL, NULL, li);
                    s = close_paren + 1;
                    continue;
                }
            }
        }
        /* Bare URL: http:// or https:// */
        if (s[0] == 'h' && (strncmp(s, "http://", 7) == 0 ||
                            strncmp(s, "https://", 8) == 0)) {
            const char *url_start = s;
            while (*s && !g_ascii_isspace(*s))
                s++;
            /* Strip trailing punctuation that's likely part of the sentence */
            const char *url_end = s;
            while (url_end > url_start) {
                char c = *(url_end - 1);
                if (c == '.' || c == ',' || c == '!' || c == '?' ||
                    c == ';' || c == ':')
                    url_end--;
                else
                    break;
            }
            append_link(out, url_start, url_end - url_start,
                       url_start, url_end - url_start,
                       NULL, NULL, li);
            /* Append any stripped trailing punctuation */
            if (url_end < s)
                g_string_append_len(out, url_end, s - url_end);
            continue;
        }
        /* Bold: **text** — recurse into content for links etc. */
        if (s[0] == '*' && s[1] == '*') {
            const char *end = strstr(s + 2, "**");
            if (end) {
                g_string_append(out, "<b>");
                char *inner = g_strndup(s + 2, end - (s + 2));
                append_inline_formatted(out, inner, li);
                g_free(inner);
                g_string_append(out, "</b>");
                s = end + 2;
                continue;
            }
        }
        /* Inline code: `text` */
        if (s[0] == '`') {
            const char *end = strchr(s + 1, '`');
            if (end) {
                const char *content = s + 1;
                gsize content_len = end - content;
                /* If code span contains only a URL, make it a clickable link */
                if ((content_len > 7 && strncmp(content, "http://", 7) == 0) ||
                    (content_len > 8 && strncmp(content, "https://", 8) == 0)) {
                    append_link(out, content, content_len,
                               content, content_len,
                               "<tt>", "</tt>", li);
                } else {
                    g_string_append(out, "<tt>");
                    g_string_append_len(out, content, content_len);
                    g_string_append(out, "</tt>");
                }
                s = end + 1;
                continue;
            }
        }
        g_string_append_c(out, *s);
        s++;
    }
}

/* Escape text for Pango markup, then apply markdown conversions */
char *markdown_to_pango_ex(const char *md, MarkdownLinkInfo *info) {
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
            GString *heading = g_string_new(NULL);
            append_inline_formatted(heading, escaped, info);
            if (level == 1)
                g_string_append_printf(out, "<b><big>%s</big></b>", heading->str);
            else
                g_string_append_printf(out, "<b>%s</b>", heading->str);
            g_string_append_c(out, '\n');
            g_string_free(heading, TRUE);
            g_free(escaped);
        } else {
            /* Check for list item: - text or * text */
            const char *trimmed = line;
            while (*trimmed == ' ') trimmed++;
            if ((*trimmed == '-' || *trimmed == '*') && trimmed[1] == ' ') {
                int indent = trimmed - line;
                char *escaped = g_markup_escape_text(trimmed + 2, -1);
                GString *item = g_string_new(NULL);
                append_inline_formatted(item, escaped, info);
                for (int i = 0; i < indent; i++)
                    g_string_append_c(out, ' ');
                g_string_append_printf(out, " \xe2\x80\xa2 %s\n", item->str);
                g_string_free(item, TRUE);
                g_free(escaped);
            } else {
                /* Regular line: escape, then apply inline formatting */
                char *escaped = g_markup_escape_text(line, -1);
                append_inline_formatted(out, escaped, info);
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

char *markdown_to_pango(const char *md) {
    return markdown_to_pango_ex(md, NULL);
}
