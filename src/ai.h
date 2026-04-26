#ifndef AI_H
#define AI_H

typedef struct {
    char *role;    /* "user" or "assistant" */
    char *content;
} AiMessage;

typedef struct AiBackend AiBackend;

struct AiBackend {
    /* Send messages with a system prompt. Returns heap-allocated response
     * string (caller must free), or NULL on error. */
    char *(*send)(AiBackend *self, const char *system_prompt,
                  AiMessage *messages, int count);
    void (*destroy)(AiBackend *self);
    void *data; /* backend-private data */
};

/* Create a Claude API backend. api_key is copied internally. */
AiBackend *ai_claude_new(const char *api_key);

/* Convenience: free a backend */
void ai_backend_free(AiBackend *backend);

#endif
