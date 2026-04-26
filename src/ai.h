#ifndef AI_H
#define AI_H

typedef struct {
    char *role;    /* "user" or "assistant" */
    char *content;
} AiMessage;

/* Streaming callback: called with each text delta, or NULL delta when done.
 * error is non-NULL only on failure. Called from the worker thread. */
typedef void (*AiStreamCallback)(const char *delta, const char *error,
                                 void *user_data);

typedef struct AiBackend AiBackend;

struct AiBackend {
    /* Stream messages with a system prompt. Calls cb with each text chunk.
     * Blocks until the response is complete. */
    void (*send_stream)(AiBackend *self, const char *system_prompt,
                        AiMessage *messages, int count,
                        AiStreamCallback cb, void *user_data);
    void (*destroy)(AiBackend *self);
    void *data; /* backend-private data */
};

/* Create a Claude API backend. api_key is copied internally. */
AiBackend *ai_claude_new(const char *api_key);

/* Convenience: free a backend */
void ai_backend_free(AiBackend *backend);

#endif
