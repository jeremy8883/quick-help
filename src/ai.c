#include "ai.h"
#include <curl/curl.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    char *api_key;
} ClaudeData;

typedef struct {
    AiStreamCallback cb;
    void *user_data;
    GString *buf;  /* partial SSE line buffer */
} StreamCtx;

/* Try to extract text delta from a parsed SSE data line */
static void process_sse_data(StreamCtx *ctx, const char *data) {
    if (strcmp(data, "[DONE]") == 0)
        return;

    GError *error = NULL;
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, data, -1, &error)) {
        g_error_free(error);
        g_object_unref(parser);
        return;
    }

    JsonObject *obj = json_node_get_object(json_parser_get_root(parser));
    const char *type = json_object_get_string_member(obj, "type");

    if (type && strcmp(type, "content_block_delta") == 0) {
        JsonObject *delta = json_object_get_object_member(obj, "delta");
        if (delta) {
            const char *text = json_object_get_string_member(delta, "text");
            if (text)
                ctx->cb(text, NULL, ctx->user_data);
        }
    } else if (type && strcmp(type, "error") == 0) {
        JsonObject *err = json_object_get_object_member(obj, "error");
        const char *msg = err ? json_object_get_string_member(err, "message") : "unknown";
        ctx->cb(NULL, msg, ctx->user_data);
    }

    g_object_unref(parser);
}

/* Process complete lines from the SSE buffer */
static void process_sse_buffer(StreamCtx *ctx) {
    char *str = ctx->buf->str;

    for (;;) {
        char *nl = strchr(str, '\n');
        if (!nl) break;

        *nl = '\0';
        /* Remove trailing \r */
        if (nl > str && *(nl - 1) == '\r')
            *(nl - 1) = '\0';

        if (strncmp(str, "data: ", 6) == 0) {
            process_sse_data(ctx, str + 6);
        }
        /* Skip event:, id:, and empty lines */

        str = nl + 1;
    }

    /* Keep any remaining partial line in the buffer */
    if (str != ctx->buf->str) {
        gsize remaining = ctx->buf->len - (str - ctx->buf->str);
        memmove(ctx->buf->str, str, remaining);
        g_string_truncate(ctx->buf, remaining);
    }
}

static size_t stream_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    StreamCtx *ctx = userdata;
    size_t total = size * nmemb;
    g_string_append_len(ctx->buf, ptr, total);
    process_sse_buffer(ctx);
    return total;
}

static void claude_send_stream(AiBackend *self, const char *system_prompt,
                               AiMessage *messages, int count,
                               AiStreamCallback cb, void *user_data) {
    ClaudeData *cd = self->data;

    /* Build JSON request body */
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);

    json_builder_set_member_name(b, "model");
    json_builder_add_string_value(b, "claude-sonnet-4-6");

    json_builder_set_member_name(b, "max_tokens");
    json_builder_add_int_value(b, 1024);

    json_builder_set_member_name(b, "stream");
    json_builder_add_boolean_value(b, TRUE);

    json_builder_set_member_name(b, "system");
    json_builder_add_string_value(b, system_prompt);

    json_builder_set_member_name(b, "messages");
    json_builder_begin_array(b);
    for (int i = 0; i < count; i++) {
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "role");
        json_builder_add_string_value(b, messages[i].role);
        json_builder_set_member_name(b, "content");
        json_builder_add_string_value(b, messages[i].content);
        json_builder_end_object(b);
    }
    json_builder_end_array(b);

    json_builder_end_object(b);

    JsonGenerator *gen = json_generator_new();
    JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(gen, root);
    gchar *body = json_generator_to_data(gen, NULL);

    json_node_unref(root);
    g_object_unref(gen);
    g_object_unref(b);

    /* HTTP request */
    CURL *curl = curl_easy_init();
    if (!curl) {
        g_free(body);
        cb(NULL, "Failed to initialize HTTP client", user_data);
        return;
    }

    StreamCtx ctx = {
        .cb = cb,
        .user_data = user_data,
        .buf = g_string_new(NULL),
    };

    struct curl_slist *headers = NULL;
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "x-api-key: %s", cd->api_key);
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "content-type: application/json");
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK)
        cb(NULL, curl_easy_strerror(res), user_data);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    g_free(body);
    g_string_free(ctx.buf, TRUE);

    /* Signal completion */
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
    return backend;
}

void ai_backend_free(AiBackend *backend) {
    if (!backend) return;
    if (backend->destroy)
        backend->destroy(backend);
    g_free(backend);
}
