#ifndef AI_SEARCH_TOOL_H
#define AI_SEARCH_TOOL_H

#include <json-glib/json-glib.h>
#include "ai_web_tool.h"

/* Perform a Brave web search, returning formatted results (caller frees). */
char *search_tool_fetch(const char *query, const char *api_key);

/* Execute a web_search tool call (caller frees result). */
char *search_tool_execute(ToolCall *call, const char *api_key);

/* Add the web_search tool definition to a JsonBuilder tools array. */
void search_tool_add_definitions(JsonBuilder *b);

#endif
