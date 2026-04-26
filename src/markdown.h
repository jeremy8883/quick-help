#ifndef MARKDOWN_H
#define MARKDOWN_H

/* Convert basic markdown text to Pango markup.
 * Supports: **bold**, `code`, ```code blocks```, # headings, - lists.
 * Returns heap-allocated string (caller must free with g_free). */
char *markdown_to_pango(const char *md);

#endif
