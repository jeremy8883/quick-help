#include "ai_search_tool.h"
#include <curl/curl.h>
#include <string.h>

#define MAX_SEARCH_RESULTS 5

/* ------------------------------------------------------------------ */
/*  Brave Search API                                                   */
/* ------------------------------------------------------------------ */

typedef struct { GString *data; } SearchBuf;

static size_t search_write_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    SearchBuf *sb = ud;
    g_string_append_len(sb->data, ptr, size * nmemb);
    return size * nmemb;
}

char *search_tool_fetch(const char *query, const char *api_key) {
    CURL *curl = curl_easy_init();
    if (!curl) return g_strdup("Error: failed to init HTTP client");

    char *escaped = curl_easy_escape(curl, query, 0);
    char *url = g_strdup_printf(
        "https://api.search.brave.com/res/v1/web/search?q=%s&count=%d",
        escaped, MAX_SEARCH_RESULTS);
    curl_free(escaped);

    char *auth = g_strdup_printf("X-Subscription-Token: %s", api_key);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, auth);
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    hdrs = curl_slist_append(hdrs, "Accept-Encoding: gzip");

    SearchBuf sb = { .data = g_string_new(NULL) };

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, search_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    CURLcode res = curl_easy_perform(curl);
    char *result;

    if (res != CURLE_OK) {
        result = g_strdup_printf("Error: %s", curl_easy_strerror(res));
    } else {
        long code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        if (code >= 400) {
            result = g_strdup_printf("HTTP error %ld", code);
        } else {
            /* Parse Brave Search JSON response */
            GError *err = NULL;
            JsonParser *parser = json_parser_new();
            if (!json_parser_load_from_data(parser, sb.data->str, -1, &err)) {
                result = g_strdup_printf("Error parsing response: %s",
                                         err->message);
                g_error_free(err);
            } else {
                JsonObject *root = json_node_get_object(
                    json_parser_get_root(parser));
                JsonObject *web = json_object_get_object_member(root, "web");
                GString *out = g_string_new(NULL);

                if (web && json_object_has_member(web, "results")) {
                    JsonArray *results_arr = json_object_get_array_member(
                        web, "results");
                    guint len = json_array_get_length(results_arr);

                    for (guint i = 0; i < len; i++) {
                        JsonObject *r = json_array_get_object_element(
                            results_arr, i);
                        const char *title = json_object_get_string_member(
                            r, "title");
                        const char *r_url = json_object_get_string_member(
                            r, "url");
                        const char *desc = json_object_get_string_member(
                            r, "description");

                        g_string_append_printf(out, "%d. %s\n   %s\n",
                                               i + 1,
                                               title ? title : "(no title)",
                                               r_url ? r_url : "");
                        if (desc && *desc)
                            g_string_append_printf(out, "   %s\n", desc);
                        g_string_append_c(out, '\n');
                    }

                    if (len == 0)
                        g_string_append(out, "No results found.");
                } else {
                    g_string_append(out, "No web results in response.");
                }
                result = g_string_free(out, FALSE);
            }
            g_object_unref(parser);
        }
    }

    g_string_free(sb.data, TRUE);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    g_free(auth);
    g_free(url);
    return result;
}

/* ------------------------------------------------------------------ */
/*  Tool execution                                                     */
/* ------------------------------------------------------------------ */

char *search_tool_execute(ToolCall *call, const char *api_key) {
    if (strcmp(call->name, "web_search") != 0)
        return g_strdup_printf("Error: unknown tool '%s'", call->name);

    JsonParser *p = json_parser_new();
    char *result;

    if (json_parser_load_from_data(p, call->input_json->str, -1, NULL)) {
        JsonObject *inp = json_node_get_object(json_parser_get_root(p));
        const char *query = json_object_get_string_member(inp, "query");
        result = query ? search_tool_fetch(query, api_key)
                       : g_strdup("Error: no query provided");
    } else {
        result = g_strdup("Error: invalid tool input");
    }

    g_object_unref(p);
    return result;
}

/* ------------------------------------------------------------------ */
/*  Tool definition                                                    */
/* ------------------------------------------------------------------ */

void search_tool_add_definitions(JsonBuilder *b) {
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, "web_search");
    json_builder_set_member_name(b, "description");
    json_builder_add_string_value(b,
        "Search the web using Brave Search. Use this when you need to find "
        "current information, look up documentation, or answer questions that "
        "require up-to-date knowledge. Returns a list of relevant results with "
        "titles, URLs, and descriptions. You can then use fetch_url to read "
        "the full content of any promising result.");
    json_builder_set_member_name(b, "input_schema");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "object");
    json_builder_set_member_name(b, "properties");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "query");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "string");
    json_builder_set_member_name(b, "description");
    json_builder_add_string_value(b, "The search query");
    json_builder_end_object(b);
    json_builder_end_object(b);
    json_builder_set_member_name(b, "required");
    json_builder_begin_array(b);
    json_builder_add_string_value(b, "query");
    json_builder_end_array(b);
    json_builder_end_object(b);
    json_builder_end_object(b);
}
