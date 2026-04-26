#ifndef AI_WEB_TOOL_H
#define AI_WEB_TOOL_H

#include <json-glib/json-glib.h>

typedef struct {
    char    *id;
    char    *name;
    GString *input_json;
} ToolCall;

/* Fetch a URL and return cleaned text content (caller frees with g_free). */
char *web_tool_fetch_url(const char *url);

/* Execute a tool call, returning the result string (caller frees). */
char *web_tool_execute(ToolCall *call);

/* Add tool definitions to a JsonBuilder (must be inside a "tools" array). */
void web_tool_add_definitions(JsonBuilder *b);

void tool_calls_free(ToolCall *calls, int n);

#endif
