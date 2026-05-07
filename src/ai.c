#include "ai.h"
#include "ai_web_tool.h"
#include "ai_search_tool.h"
#include <curl/curl.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdlib.h>

#define MAX_TOOL_ROUNDS 5

typedef struct {
    char *api_key;
} ClaudeData;

typedef struct {
    AiStreamCallback cb;
    void            *user_data;
    GString         *buf;         /* SSE line buffer */
    GString         *full_resp;   /* raw response (for non-SSE error parsing) */
    GString         *full_text;   /* accumulated text from text blocks */
    ToolCall        *tool_calls;
    int              num_tools;
    int              tool_cap;
    char            *stop_reason;
    int             *cancel_flag;
    gboolean         had_error;   /* TRUE if error callback already fired */
    AiToolStatusCallback tool_status_cb;
    void                *tool_status_data;
} StreamState;

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
            if (bt && strcmp(bt, "server_tool_use") == 0) {
                const char *name = json_object_get_string_member(cb, "name");
                if (name && strcmp(name, "web_search") == 0
                    && st->tool_status_cb) {
                    st->tool_status_cb("server_web_search", "",
                                       st->tool_status_data);
                }
            } else if (bt && strcmp(bt, "tool_use") == 0) {
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
        st->had_error = TRUE;
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
    if (g_atomic_int_get(st->cancel_flag))
        return 0; /* abort curl transfer */
    size_t total = size * nmemb;
    g_string_append_len(st->buf, ptr, total);
    g_string_append_len(st->full_resp, ptr, total);
    process_sse_buffer(st);
    return total;
}

/* ------------------------------------------------------------------ */
/*  JSON helpers for message building                                  */
/* ------------------------------------------------------------------ */

/* Strip UI tool markers (\x01TOOL:...\x01) from message content */
static char *strip_tool_markers(const char *text) {
    if (!text || !strchr(text, '\x01')) return g_strdup(text);
    GString *out = g_string_new(NULL);
    const char *p = text;
    while (*p) {
        if (*p == '\x01' && strncmp(p, "\x01TOOL:", 6) == 0) {
            const char *end = strchr(p + 6, '\x01');
            if (end) { p = end + 1; continue; }
        }
        if (*p != '\x01')
            g_string_append_c(out, *p);
        p++;
    }
    return g_string_free(out, FALSE);
}

static JsonNode *make_msg(const char *role, const char *content,
                          AiImage *images, int image_count) {
    char *clean = strip_tool_markers(content);
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "role");
    json_builder_add_string_value(b, role);
    json_builder_set_member_name(b, "content");

    if (image_count > 0) {
        /* Content blocks array: images first, then text */
        json_builder_begin_array(b);
        for (int i = 0; i < image_count; i++) {
            json_builder_begin_object(b);
            json_builder_set_member_name(b, "type");
            json_builder_add_string_value(b, "image");
            json_builder_set_member_name(b, "source");
            json_builder_begin_object(b);
            json_builder_set_member_name(b, "type");
            json_builder_add_string_value(b, "base64");
            json_builder_set_member_name(b, "media_type");
            json_builder_add_string_value(b, images[i].media_type);
            json_builder_set_member_name(b, "data");
            json_builder_add_string_value(b, images[i].base64);
            json_builder_end_object(b); /* source */
            json_builder_end_object(b); /* image block */
        }
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "type");
        json_builder_add_string_value(b, "text");
        json_builder_set_member_name(b, "text");
        json_builder_add_string_value(b, clean);
        json_builder_end_object(b);
        json_builder_end_array(b);
    } else {
        json_builder_add_string_value(b, clean);
    }

    json_builder_end_object(b);
    g_free(clean);
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

/* ------------------------------------------------------------------ */
/*  Tool dispatch                                                      */
/* ------------------------------------------------------------------ */

static char *execute_tool(ToolCall *call, AiBackend *self) {
    if (strcmp(call->name, "fetch_url") == 0)
        return web_tool_execute(call);
    if (strcmp(call->name, "web_search") == 0 && self->brave_api_key)
        return search_tool_execute(call, self->brave_api_key);
    return g_strdup_printf("Error: unknown tool '%s'", call->name);
}

typedef struct {
    ToolCall  *call;
    AiBackend *backend;
    char      *result;
} ToolExecArgs;

static gpointer tool_exec_thread(gpointer data) {
    ToolExecArgs *args = data;
    args->result = execute_tool(args->call, args->backend);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Claude send (with tool-use loop)                                   */
/* ------------------------------------------------------------------ */

static void claude_send_stream(AiBackend *self, const char *system_prompt,
                               AiMessage *messages, int count,
                               AiStreamCallback cb, void *user_data) {
    ClaudeData *cd = self->data;

    GPtrArray *msgs = g_ptr_array_new_with_free_func(
        (GDestroyNotify)json_node_unref);
    for (int i = 0; i < count; i++)
        g_ptr_array_add(msgs, make_msg(messages[i].role,
                                       messages[i].content,
                                       messages[i].images,
                                       messages[i].image_count));

    g_atomic_int_set(&self->cancel_requested, 0);

    for (int round = 0; round < MAX_TOOL_ROUNDS; round++) {

        /* ---- Build request JSON ---- */
        JsonBuilder *b = json_builder_new();
        json_builder_begin_object(b);

        json_builder_set_member_name(b, "model");
        json_builder_add_string_value(b, self->model);
        json_builder_set_member_name(b, "max_tokens");
        json_builder_add_int_value(b, 4096);
        json_builder_set_member_name(b, "stream");
        json_builder_add_boolean_value(b, TRUE);
        json_builder_set_member_name(b, "system");
        json_builder_add_string_value(b, system_prompt);

        json_builder_set_member_name(b, "tools");
        json_builder_begin_array(b);
        web_tool_add_definitions(b);
        if (self->brave_api_key)
            search_tool_add_definitions(b);
        else {
            /* Anthropic server-side web search (no API key needed) */
            json_builder_begin_object(b);
            json_builder_set_member_name(b, "type");
            json_builder_add_string_value(b, "web_search_20250305");
            json_builder_set_member_name(b, "name");
            json_builder_add_string_value(b, "web_search");
            json_builder_set_member_name(b, "max_uses");
            json_builder_add_int_value(b, 5);
            json_builder_end_object(b);
        }
        json_builder_end_array(b);

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
            .cb               = cb,
            .user_data        = user_data,
            .buf              = g_string_new(NULL),
            .full_resp        = g_string_new(NULL),
            .full_text        = g_string_new(NULL),
            .cancel_flag      = &self->cancel_requested,
            .tool_status_cb   = self->tool_status_cb,
            .tool_status_data = self->tool_status_data,
        };

        char *auth = g_strdup_printf("x-api-key: %s", cd->api_key);
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
        gboolean cancelled = g_atomic_int_get(&self->cancel_requested);
        if (res != CURLE_OK && !cancelled)
            cb(NULL, curl_easy_strerror(res), user_data);

        /* Check HTTP status for non-SSE error responses (e.g. 400 with
         * plain JSON body).  The SSE parser only handles "data:" lines,
         * so a plain JSON error body is silently ignored. */
        if (res == CURLE_OK && !cancelled && !st.had_error) {
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            if (http_code >= 400) {
                /* Try to extract the message from the JSON error body */
                const char *err_msg = NULL;
                JsonParser *ep = json_parser_new();
                if (json_parser_load_from_data(ep, st.full_resp->str,
                                               st.full_resp->len, NULL)) {
                    JsonObject *eo = json_node_get_object(
                        json_parser_get_root(ep));
                    JsonObject *ei = eo ? json_object_get_object_member(
                        eo, "error") : NULL;
                    if (ei)
                        err_msg = json_object_get_string_member(
                            ei, "message");
                }
                if (err_msg) {
                    cb(NULL, err_msg, user_data);
                } else {
                    char *fallback = g_strdup_printf(
                        "API request failed (HTTP %ld)", http_code);
                    cb(NULL, fallback, user_data);
                    g_free(fallback);
                }
                g_object_unref(ep);
            }
        }

        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);
        g_free(auth);
        g_free(body);
        g_string_free(st.buf, TRUE);
        g_string_free(st.full_resp, TRUE);

        /* ---- Handle tool calls ---- */
        gboolean need_tools = !cancelled && st.stop_reason
            && strcmp(st.stop_reason, "tool_use") == 0
            && st.num_tools > 0;

        if (need_tools) {
            g_ptr_array_add(msgs, make_assistant_tool_msg(
                st.full_text->str, st.tool_calls, st.num_tools));

            /* Notify UI about all pending tool calls */
            if (self->tool_status_cb) {
                for (int i = 0; i < st.num_tools; i++) {
                    char *detail = NULL;
                    JsonParser *tp = json_parser_new();
                    if (json_parser_load_from_data(tp,
                            st.tool_calls[i].input_json->str, -1, NULL)) {
                        JsonObject *inp = json_node_get_object(
                            json_parser_get_root(tp));
                        const char *val = json_object_get_string_member(
                            inp, "url");
                        if (!val)
                            val = json_object_get_string_member(inp, "query");
                        if (val) detail = g_strdup(val);
                    }
                    g_object_unref(tp);
                    self->tool_status_cb(st.tool_calls[i].name,
                                         detail ? detail : "",
                                         self->tool_status_data);
                    g_free(detail);
                }
            }

            /* Execute tool calls in parallel */
            char **results = g_new0(char *, st.num_tools);
            if (st.num_tools == 1) {
                results[0] = execute_tool(&st.tool_calls[0], self);
            } else {
                ToolExecArgs *args = g_new0(ToolExecArgs, st.num_tools);
                GThread **threads = g_new(GThread *, st.num_tools);
                for (int i = 0; i < st.num_tools; i++) {
                    args[i].call = &st.tool_calls[i];
                    args[i].backend = self;
                    threads[i] = g_thread_new(NULL, tool_exec_thread, &args[i]);
                }
                for (int i = 0; i < st.num_tools; i++) {
                    g_thread_join(threads[i]);
                    results[i] = args[i].result;
                }
                g_free(threads);
                g_free(args);
            }

            g_ptr_array_add(msgs, make_tool_result_msg(
                st.tool_calls, st.num_tools, results));

            for (int i = 0; i < st.num_tools; i++) g_free(results[i]);
            g_free(results);
        }

        tool_calls_free(st.tool_calls, st.num_tools);
        g_string_free(st.full_text, TRUE);
        g_free(st.stop_reason);

        if (!need_tools) break;
    }

    g_ptr_array_unref(msgs);
    cb(NULL, NULL, user_data);
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
    backend->model = g_strdup("claude-sonnet-4-6");
    return backend;
}

void ai_backend_free(AiBackend *backend) {
    if (!backend) return;
    g_free(backend->model);
    g_free(backend->brave_api_key);
    if (backend->destroy)
        backend->destroy(backend);
    g_free(backend);
}
