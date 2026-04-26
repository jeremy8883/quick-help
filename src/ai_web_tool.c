#include "ai_web_tool.h"
#include "utils/html.h"
#include <curl/curl.h>
#include <string.h>

#define MAX_FETCH_BYTES   200000
#define MAX_CONTENT_CHARS 12000

/* ------------------------------------------------------------------ */
/*  URL fetching                                                       */
/* ------------------------------------------------------------------ */

typedef struct { GString *data; size_t max; } FetchBuf;

static size_t fetch_write_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    FetchBuf *fb = ud;
    size_t total = size * nmemb;
    size_t take = (fb->data->len + total > fb->max)
                  ? fb->max - fb->data->len : total;
    if (take > 0) g_string_append_len(fb->data, ptr, take);
    return total;
}

char *web_tool_fetch_url(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) return g_strdup("Error: failed to init HTTP client");

    FetchBuf fb = { .data = g_string_new(NULL), .max = MAX_FETCH_BYTES };

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fetch_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &fb);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "QuickHelp/1.0");
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
            char *ct = NULL;
            curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
            if (ct && (strstr(ct, "text/html") || strstr(ct, "xhtml"))) {
                result = html_strip_tags(fb.data->str, fb.data->len,
                                         MAX_CONTENT_CHARS);
            } else {
                if (fb.data->len > MAX_CONTENT_CHARS) {
                    g_string_truncate(fb.data, MAX_CONTENT_CHARS);
                    g_string_append(fb.data, "\n[content truncated]");
                }
                result = g_string_free(fb.data, FALSE);
                fb.data = NULL;
            }
        }
    }

    if (fb.data) g_string_free(fb.data, TRUE);
    curl_easy_cleanup(curl);
    return result;
}

/* ------------------------------------------------------------------ */
/*  Tool execution                                                     */
/* ------------------------------------------------------------------ */

char *web_tool_execute(ToolCall *call) {
    if (strcmp(call->name, "fetch_url") != 0)
        return g_strdup_printf("Error: unknown tool '%s'", call->name);

    JsonParser *p = json_parser_new();
    char *result;

    if (json_parser_load_from_data(p, call->input_json->str, -1, NULL)) {
        JsonObject *inp = json_node_get_object(json_parser_get_root(p));
        const char *url = json_object_get_string_member(inp, "url");
        result = url ? web_tool_fetch_url(url)
                     : g_strdup("Error: no URL provided");
    } else {
        result = g_strdup("Error: invalid tool input");
    }

    g_object_unref(p);
    return result;
}

/* ------------------------------------------------------------------ */
/*  Tool definition                                                    */
/* ------------------------------------------------------------------ */

void web_tool_add_definitions(JsonBuilder *b) {
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, "fetch_url");
    json_builder_set_member_name(b, "description");
    json_builder_add_string_value(b,
        "Fetch the contents of a web page. Use this to retrieve current "
        "information from URLs you know about (documentation, references, "
        "release notes, etc). You can call this multiple times to follow links.");
    json_builder_set_member_name(b, "input_schema");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "object");
    json_builder_set_member_name(b, "properties");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "url");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "string");
    json_builder_set_member_name(b, "description");
    json_builder_add_string_value(b, "The URL to fetch");
    json_builder_end_object(b);
    json_builder_end_object(b);
    json_builder_set_member_name(b, "required");
    json_builder_begin_array(b);
    json_builder_add_string_value(b, "url");
    json_builder_end_array(b);
    json_builder_end_object(b);
    json_builder_end_object(b);
}

/* ------------------------------------------------------------------ */
/*  Cleanup                                                            */
/* ------------------------------------------------------------------ */

void tool_calls_free(ToolCall *calls, int n) {
    for (int i = 0; i < n; i++) {
        g_free(calls[i].id);
        g_free(calls[i].name);
        if (calls[i].input_json) g_string_free(calls[i].input_json, TRUE);
    }
    g_free(calls);
}
