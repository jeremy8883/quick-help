#ifndef MARKDOWN_H
#define MARKDOWN_H

#include <glib.h>

/* Link tracking/highlighting for extended markdown rendering.
 * Initialize current_link to 0 before the first call.
 * The struct can be reused across multiple calls to accumulate links. */
typedef struct {
    int highlight_index;  /* link index to highlight (-1 for none) */
    int current_link;     /* running counter (init to 0) */
    GPtrArray *urls;      /* if non-NULL, collects unescaped URLs */
} MarkdownLinkInfo;

/* Convert basic markdown text to Pango markup.
 * Supports: **bold**, `code`, ```code blocks```, # headings, - lists,
 * [text](url) links, and bare http(s) URLs.
 * Returns heap-allocated string (caller must free with g_free). */
char *markdown_to_pango(const char *md);

/* Extended version with link tracking and highlighting. */
char *markdown_to_pango_ex(const char *md, MarkdownLinkInfo *info);

#endif
