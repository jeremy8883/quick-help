#ifndef WINDOW_INTERNAL_H
#define WINDOW_INTERNAL_H

#include "window.h"
#include "ai.h"
#include "markdown.h"

#define INITIAL_WIDTH  500
#define INITIAL_HEIGHT 60
#define EXPANDED_HEIGHT 500
#define MAX_MESSAGES 50
#define SCROLL_STEP 60.0
#define IMAGE_THUMB_SIZE 70

typedef struct {
    GdkTexture *texture;
    char *media_type;
    char *base64;
} PendingImage;

struct _QuickHelpWindow {
    GtkWindow *window;
    GtkTextView *text_view;
    GtkBox *chat_box;           /* vertical box inside scroll for messages */
    GtkScrolledWindow *scroll;
    GtkProgressBar *progress_bar;
    guint pulse_timer_id;
    GtkBox *vbox;
    AiBackend *backend;
    WindowInfo *info;
    SystemContext *sys;
    AiMessage messages[MAX_MESSAGES];
    int msg_count;
    char *system_prompt;
    gboolean expanded;
    GString *streaming_buf;  /* accumulates assistant response during streaming */
    gboolean streaming;      /* TRUE while a response is being streamed */
    GMutex stream_lock;      /* protects streaming_buf */
    gboolean stream_ui_pending; /* whether an idle update is already queued */
    /* (tool status is stored inline in streaming_buf as \x01TOOL:...\x01 markers) */
    int focused_bubble;      /* currently focused assistant bubble (-1 = none) */
    int bubble_count;        /* number of assistant bubbles in last render */
    GPtrArray *bubble_links; /* GPtrArray of GPtrArray* of link URLs per bubble */
    GtkDropDown *model_dropdown;
    GtkButton *stop_button;  /* shown during streaming */
    GtkLabel *error_label;   /* red error message below entry */
    gboolean stream_had_error;  /* protected by stream_lock */
    gboolean stream_cancelled;  /* protected by stream_lock */
    char *stream_error_msg;     /* protected by stream_lock */
    GPtrArray *pending_images;  /* array of PendingImage* */
    GtkBox *image_preview_box;  /* horizontal box for thumbnails */
    GtkButton *scroll_to_bottom; /* floating chevron button */
    GtkWidget *drop_overlay;     /* "Drop images here" overlay */
    gboolean scroll_pin_bottom;  /* request scroll-to-bottom on next upper change */
    double scroll_pin_value;     /* restore this value on next upper change (-1=none) */
    gboolean submit_after_cancel; /* submit queued text after stream cancel completes */
};

/* Functions shared between window.c and input.c */
char *get_input_text(QuickHelpWindow *qh);
void set_input_text(QuickHelpWindow *qh, const char *text);
void on_submit(QuickHelpWindow *qh);
void cancel_stream(QuickHelpWindow *qh);
void render_conversation(QuickHelpWindow *qh, const char *partial_assistant);
void scroll_to_bottom(QuickHelpWindow *qh);
void show_link_picker(QuickHelpWindow *qh, GPtrArray *links);
void clear_conversation(QuickHelpWindow *qh);
void paste_clipboard_image(QuickHelpWindow *qh);

/* Key handler (defined in input.c, used by window.c) */
gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                        guint keycode, GdkModifierType state, gpointer data);

#endif
