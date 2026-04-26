#include "ai.h"
#include <curl/curl.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdlib.h>

#define MAX_TOOL_ROUNDS   5
#define MAX_FETCH_BYTES   200000
#define MAX_CONTENT_CHARS 12000

typedef struct {
    char *api_key;
} ClaudeData;

typedef struct {
    char    *id;
    char    *name;
    GString *input_json;
} ToolCall;

typedef struct {
    AiStreamCallback cb;
    void            *user_data;
    GString         *buf;         /* SSE line buffer */
    GString         *full_text;   /* accumulated text from text blocks */
    ToolCall        *tool_calls;
    int              num_tools;
    int              tool_cap;
    char            *stop_reason;
} StreamState;

/* ------------------------------------------------------------------ */
/*  HTML stripping                                                     */
/* ------------------------------------------------------------------ */

static char *strip_html(const char *html, size_t len) {
    GString *out = g_string_sized_new(len / 3);
    const char *p = html, *end = html + len;
    gboolean in_script = FALSE, in_style = FALSE;
    int blank_lines = 0;

    while (p < end) {
        if (*p == '<') {
            const char *gt = memchr(p, '>', end - p);
            if (!gt) break;
            const char *tag = p + 1;
            gboolean closing = (*tag == '/');
            const char *name = closing ? tag + 1 : tag;
            while (name < gt && *name == ' ') name++;

            if (g_ascii_strncasecmp(name, "script", 6) == 0)
                in_script = !closing;
            else if (g_ascii_strncasecmp(name, "style", 5) == 0)
                in_style = !closing;

            if (!in_script && !in_style &&
                (g_ascii_strncasecmp(name, "br", 2) == 0 ||
                 g_ascii_strncasecmp(name, "p",  1) == 0 ||
                 g_ascii_strncasecmp(name, "div",3) == 0 ||
                 g_ascii_strncasecmp(name, "li", 2) == 0 ||
                 g_ascii_strncasecmp(name, "tr", 2) == 0 ||
                 g_ascii_strncasecmp(name, "h1", 2) == 0 ||
                 g_ascii_strncasecmp(name, "h2", 2) == 0 ||
                 g_ascii_strncasecmp(name, "h3", 2) == 0 ||
                 g_ascii_strncasecmp(name, "h4", 2) == 0 ||
                 g_ascii_strncasecmp(name, "h5", 2) == 0 ||
                 g_ascii_strncasecmp(name, "h6", 2) == 0)) {
                if (blank_lines < 2) {
                    g_string_append_c(out, '\n');
                    blank_lines++;
                }
            }
            p = gt + 1;
        } else if (in_script || in_style) {
            p++;
        } else if (*p == '&') {
            if (g_str_has_prefix(p, "&amp;"))       { g_string_append_c(out, '&');  p += 5; }
            else if (g_str_has_prefix(p, "&lt;"))    { g_string_append_c(out, '<');  p += 4; }
            else if (g_str_has_prefix(p, "&gt;"))    { g_string_append_c(out, '>');  p += 4; }
            else if (g_str_has_prefix(p, "&quot;"))  { g_string_append_c(out, '"');  p += 6; }
            else if (g_str_has_prefix(p, "&nbsp;"))  { g_string_append_c(out, ' ');  p += 6; }
            else if (g_str_has_prefix(p, "&#")) {
                const char *semi = memchr(p, ';', MIN((size_t)(end - p), 10));
                p = semi ? semi + 1 : p + 1;
            } else { g_string_append_c(out, '&'); p++; }
            blank_lines = 0;
        } else if (*p == '\n' || *p == '\r') {
            if (blank_lines < 2) { g_string_append_c(out, '\n'); blank_lines++; }
            p++;
        } else if (*p == ' ' || *p == '\t') {
            if (out->len > 0 && out->str[out->len-1] != ' ' && out->str[out->len-1] != '\n')
                g_string_append_c(out, ' ');
            p++;
        } else {
            g_string_append_c(out, *p);
            blank_lines = 0;
            p++;
        }
        if (out->len > MAX_CONTENT_CHARS) {
            g_string_append(out, "\n[content truncated]");
            break;
        }
    }
    return g_string_free(out, FALSE);
}

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

static char *fetch_url(const char *url) {
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
                result = strip_html(fb.data->str, fb.data->len);
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
/*  SSE processing                                                     */
/* ------------------------------------------------------------------ */

static void process_sse_data(StreamState *st, const char *data) {
    if (strcmp(data, "[DONE]") == 0) return;

    GError *err = NULL;
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, data, -1, &err)) {
        g_error_free(err);
        g_object_unref(parser);
        return;
    }

    JsonObject *obj = json_node_get_object(json_parser_get_root(parser));
    const char *type = json_object_get_string_member(obj, "type");

    if (type && strcmp(type, "content_block_start") == 0) {
        JsonObject *cb = json_object_get_object_member(obj, "content_block");
        if (cb) {
            const char *bt = json_object_get_string_member(cb, "type");
            if (bt && strcmp(bt, "tool_use") == 0) {
                if (st->num_tools >= st->tool_cap) {
                    st->tool_cap = st->tool_cap ? st->tool_cap * 2 : 4;
                    st->tool_calls = g_realloc(st->tool_calls,
                                               sizeof(ToolCall) * st->tool_cap);
                }
                ToolCall *tc = &st->tool_calls[st->num_tools++];
                tc->id   = g_strdup(json_object_get_string_member(cb, "id"));
                tc->name = g_strdup(json_object_get_string_member(cb, "name"));
                tc->input_json = g_string_new(NULL);
            }
        }
    } else if (type && strcmp(type, "content_block_delta") == 0) {
        JsonObject *delta = json_object_get_object_member(obj, "delta");
        if (delta) {
            const char *dt = json_object_get_string_member(delta, "type");
            if (dt && strcmp(dt, "text_delta") == 0) {
                const char *text = json_object_get_string_member(delta, "text");
                if (text) {
                    st->cb(text, NULL, st->user_data);
                    g_string_append(st->full_text, text);
                }
            } else if (dt && strcmp(dt, "input_json_delta") == 0) {
                const char *pj = json_object_get_string_member(delta, "partial_json");
                if (pj && st->num_tools > 0)
                    g_string_append(st->tool_calls[st->num_tools - 1].input_json, pj);
            }
        }
    } else if (type && strcmp(type, "message_delta") == 0) {
        JsonObject *delta = json_object_get_object_member(obj, "delta");
        if (delta) {
            const char *sr = json_object_get_string_member(delta, "stop_reason");
            if (sr) { g_free(st->stop_reason); st->stop_reason = g_strdup(sr); }
        }
    } else if (type && strcmp(type, "error") == 0) {
        JsonObject *e = json_object_get_object_member(obj, "error");
        const char *msg = e ? json_object_get_string_member(e, "message") : "unknown";
        st->cb(NULL, msg, st->user_data);
    }

    g_object_unref(parser);
}

static void process_sse_buffer(StreamState *st) {
    char *str = st->buf->str;
    for (;;) {
        char *nl = strchr(str, '\n');
        if (!nl) break;
        *nl = '\0';
        if (nl > str && *(nl - 1) == '\r') *(nl - 1) = '\0';
        if (strncmp(str, "data: ", 6) == 0)
            process_sse_data(st, str + 6);
        str = nl + 1;
    }
    if (str != st->buf->str) {
        gsize rem = st->buf->len - (str - st->buf->str);
        memmove(st->buf->str, str, rem);
        g_string_truncate(st->buf, rem);
    }
}

static size_t stream_write_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    StreamState *st = ud;
    size_t total = size * nmemb;
    g_string_append_len(st->buf, ptr, total);
    process_sse_buffer(st);
    return total;
}

/* ------------------------------------------------------------------ */
/*  JSON helpers for tool-use message building                         */
/* ------------------------------------------------------------------ */

static JsonNode *make_simple_msg(const char *role, const char *content) {
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "role");
    json_builder_add_string_value(b, role);
    json_builder_set_member_name(b, "content");
    json_builder_add_string_value(b, content);
    json_builder_end_object(b);
    JsonNode *n = json_builder_get_root(b);
    g_object_unref(b);
    return n;
}

static JsonNode *make_assistant_tool_msg(const char *text,
                                         ToolCall *calls, int n) {
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "role");
    json_builder_add_string_value(b, "assistant");
    json_builder_set_member_name(b, "content");
    json_builder_begin_array(b);

    if (text && *text) {
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "type");
        json_builder_add_string_value(b, "text");
        json_builder_set_member_name(b, "text");
        json_builder_add_string_value(b, text);
        json_builder_end_object(b);
    }

    for (int i = 0; i < n; i++) {
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "type");
        json_builder_add_string_value(b, "tool_use");
        json_builder_set_member_name(b, "id");
        json_builder_add_string_value(b, calls[i].id);
        json_builder_set_member_name(b, "name");
        json_builder_add_string_value(b, calls[i].name);
        json_builder_set_member_name(b, "input");
        /* Parse accumulated JSON string into a node */
        JsonParser *p = json_parser_new();
        if (json_parser_load_from_data(p, calls[i].input_json->str, -1, NULL))
            json_builder_add_value(b, json_node_copy(json_parser_get_root(p)));
        else {
            json_builder_begin_object(b);
            json_builder_end_object(b);
        }
        g_object_unref(p);
        json_builder_end_object(b);
    }

    json_builder_end_array(b);
    json_builder_end_object(b);
    JsonNode *node = json_builder_get_root(b);
    g_object_unref(b);
    return node;
}

static JsonNode *make_tool_result_msg(ToolCall *calls, int n, char **results) {
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "role");
    json_builder_add_string_value(b, "user");
    json_builder_set_member_name(b, "content");
    json_builder_begin_array(b);

    for (int i = 0; i < n; i++) {
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "type");
        json_builder_add_string_value(b, "tool_result");
        json_builder_set_member_name(b, "tool_use_id");
        json_builder_add_string_value(b, calls[i].id);
        json_builder_set_member_name(b, "content");
        json_builder_add_string_value(b, results[i]);
        json_builder_end_object(b);
    }

    json_builder_end_array(b);
    json_builder_end_object(b);
    JsonNode *node = json_builder_get_root(b);
    g_object_unref(b);
    return node;
}

static void free_tool_calls(ToolCall *calls, int n) {
    for (int i = 0; i < n; i++) {
        g_free(calls[i].id);
        g_free(calls[i].name);
        if (calls[i].input_json) g_string_free(calls[i].input_json, TRUE);
    }
    g_free(calls);
}

/* ------------------------------------------------------------------ */
/*  Claude send (with tool-use loop)                                   */
/* ------------------------------------------------------------------ */

static void claude_send_stream(AiBackend *self, const char *system_prompt,
                               AiMessage *messages, int count,
                               AiStreamCallback cb, void *user_data) {
    ClaudeData *cd = self->data;

    /* Collect all message nodes (originals + tool-loop extras) */
    GPtrArray *msgs = g_ptr_array_new_with_free_func(
        (GDestroyNotify)json_node_unref);
    for (int i = 0; i < count; i++)
        g_ptr_array_add(msgs, make_simple_msg(messages[i].role,
                                              messages[i].content));

    for (int round = 0; round < MAX_TOOL_ROUNDS; round++) {

        /* ---- Build request JSON ---- */
        JsonBuilder *b = json_builder_new();
        json_builder_begin_object(b);

        json_builder_set_member_name(b, "model");
        json_builder_add_string_value(b, "claude-sonnet-4-6");
        json_builder_set_member_name(b, "max_tokens");
        json_builder_add_int_value(b, 4096);
        json_builder_set_member_name(b, "stream");
        json_builder_add_boolean_value(b, TRUE);
        json_builder_set_member_name(b, "system");
        json_builder_add_string_value(b, system_prompt);

        /* Tool definitions */
        json_builder_set_member_name(b, "tools");
        json_builder_begin_array(b);
        {
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
        json_builder_end_array(b);

        /* Messages */
        json_builder_set_member_name(b, "messages");
        json_builder_begin_array(b);
        for (guint i = 0; i < msgs->len; i++)
            json_builder_add_value(b,
                json_node_copy(g_ptr_array_index(msgs, i)));
        json_builder_end_array(b);

        json_builder_end_object(b);

        JsonGenerator *gen = json_generator_new();
        JsonNode *root = json_builder_get_root(b);
        json_generator_set_root(gen, root);
        gchar *body = json_generator_to_data(gen, NULL);
        json_node_unref(root);
        g_object_unref(gen);
        g_object_unref(b);

        /* ---- HTTP request ---- */
        CURL *curl = curl_easy_init();
        if (!curl) {
            g_free(body);
            cb(NULL, "Failed to initialize HTTP client", user_data);
            break;
        }

        StreamState st = {
            .cb        = cb,
            .user_data = user_data,
            .buf       = g_string_new(NULL),
            .full_text = g_string_new(NULL),
        };

        char auth[512];
        snprintf(auth, sizeof(auth), "x-api-key: %s", cd->api_key);
        struct curl_slist *hdrs = NULL;
        hdrs = curl_slist_append(hdrs, auth);
        hdrs = curl_slist_append(hdrs, "content-type: application/json");
        hdrs = curl_slist_append(hdrs, "anthropic-version: 2023-06-01");

        curl_easy_setopt(curl, CURLOPT_URL,
                         "https://api.anthropic.com/v1/messages");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &st);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK)
            cb(NULL, curl_easy_strerror(res), user_data);

        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);
        g_free(body);
        g_string_free(st.buf, TRUE);

        /* ---- Handle tool calls ---- */
        gboolean need_tools = st.stop_reason
            && strcmp(st.stop_reason, "tool_use") == 0
            && st.num_tools > 0;

        if (need_tools) {
            /* Record the assistant's response (text + tool_use blocks) */
            g_ptr_array_add(msgs, make_assistant_tool_msg(
                st.full_text->str, st.tool_calls, st.num_tools));

            /* Execute each tool */
            char **results = g_new0(char *, st.num_tools);
            for (int i = 0; i < st.num_tools; i++) {
                if (strcmp(st.tool_calls[i].name, "fetch_url") == 0) {
                    JsonParser *p = json_parser_new();
                    if (json_parser_load_from_data(
                            p, st.tool_calls[i].input_json->str, -1, NULL)) {
                        JsonObject *inp = json_node_get_object(
                            json_parser_get_root(p));
                        const char *url =
                            json_object_get_string_member(inp, "url");
                        results[i] = url ? fetch_url(url)
                                         : g_strdup("Error: no URL provided");
                    } else {
                        results[i] = g_strdup("Error: invalid tool input");
                    }
                    g_object_unref(p);
                } else {
                    results[i] = g_strdup_printf(
                        "Error: unknown tool '%s'", st.tool_calls[i].name);
                }
            }

            /* Send tool results back */
            g_ptr_array_add(msgs, make_tool_result_msg(
                st.tool_calls, st.num_tools, results));

            for (int i = 0; i < st.num_tools; i++) g_free(results[i]);
            g_free(results);
        }

        free_tool_calls(st.tool_calls, st.num_tools);
        g_string_free(st.full_text, TRUE);
        g_free(st.stop_reason);

        if (!need_tools) break;   /* done — no more tool calls */
    }

    g_ptr_array_unref(msgs);
    cb(NULL, NULL, user_data);  /* signal completion */
}

static void claude_destroy(AiBackend *self) {
    ClaudeData *cd = self->data;
    g_free(cd->api_key);
    g_free(cd);
}

AiBackend *ai_claude_new(const char *api_key) {
    AiBackend *backend = g_new0(AiBackend, 1);
    ClaudeData *cd = g_new0(ClaudeData, 1);
    cd->api_key = g_strdup(api_key);
    backend->data = cd;
    backend->send_stream = claude_send_stream;
    backend->destroy = claude_destroy;
    return backend;
}

void ai_backend_free(AiBackend *backend) {
    if (!backend) return;
    if (backend->destroy)
        backend->destroy(backend);
    g_free(backend);
}
