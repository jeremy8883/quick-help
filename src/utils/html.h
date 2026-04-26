#ifndef UTILS_HTML_H
#define UTILS_HTML_H

#include <glib.h>
#include <stddef.h>

/* Strip HTML tags, script/style blocks, and decode common entities.
 * Returns cleaned plain text, truncated to max_chars. Caller frees with g_free. */
char *html_strip_tags(const char *html, size_t len, size_t max_chars);

#endif
