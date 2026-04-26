#include "ai.h"
#include <curl/curl.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    char *api_key;
} ClaudeData;

typedef struct {
    char *data;
    size_t len;
} Buffer;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    Buffer *buf = userdata;
    size_t total = size * nmemb;
    char *tmp = realloc(buf->data, buf->len + total + 1);
    if (!tmp) return 0;
    buf->data = tmp;
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

static char *claude_send(AiBackend *self, const char *system_prompt,
                         AiMessage *messages, int count) {
    ClaudeData *cd = self->data;

    /* Build JSON request body */
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);

    json_builder_set_member_name(b, "model");
    json_builder_add_string_value(b, "claude-sonnet-4-6");

    json_builder_set_member_name(b, "max_tokens");
    json_builder_add_int_value(b, 1024);

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
        return NULL;
    }

    Buffer response = {0};
    struct curl_slist *headers = NULL;
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "x-api-key: %s", cd->api_key);
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "content-type: application/json");
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    g_free(body);

    if (res != CURLE_OK) {
        free(response.data);
        return g_strdup_printf("Error: %s", curl_easy_strerror(res));
    }

    /* Parse response JSON */
    GError *error = NULL;
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, response.data, response.len, &error)) {
        char *err = g_strdup_printf("Failed to parse response: %s", error->message);
        g_error_free(error);
        g_object_unref(parser);
        free(response.data);
        return err;
    }

    JsonObject *obj = json_node_get_object(json_parser_get_root(parser));

    /* Check for API error */
    if (json_object_has_member(obj, "error")) {
        JsonObject *err_obj = json_object_get_object_member(obj, "error");
        const char *msg = json_object_get_string_member(err_obj, "message");
        char *err = g_strdup_printf("API Error: %s", msg ? msg : "unknown");
        g_object_unref(parser);
        free(response.data);
        return err;
    }

    /* Extract text from content[0].text */
    char *result_text = NULL;
    if (json_object_has_member(obj, "content")) {
        JsonArray *content = json_object_get_array_member(obj, "content");
        if (json_array_get_length(content) > 0) {
            JsonObject *block = json_array_get_object_element(content, 0);
            const char *text = json_object_get_string_member(block, "text");
            if (text)
                result_text = g_strdup(text);
        }
    }

    if (!result_text)
        result_text = g_strdup("(No response text)");

    g_object_unref(parser);
    free(response.data);
    return result_text;
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
    backend->send = claude_send;
    backend->destroy = claude_destroy;
    return backend;
}

void ai_backend_free(AiBackend *backend) {
    if (!backend) return;
    if (backend->destroy)
        backend->destroy(backend);
    g_free(backend);
}
