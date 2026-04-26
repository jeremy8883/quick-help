#include "window.h"
#include "system_context.h"
#include "markdown.h"
#include <string.h>

#define INITIAL_WIDTH  500
#define INITIAL_HEIGHT 60
#define EXPANDED_HEIGHT 500
#define MAX_MESSAGES 50
#define SCROLL_STEP 60.0

static const char *model_ids[] = {
    "claude-haiku-4-5-20251001",
    "claude-sonnet-4-6",
    "claude-opus-4-6",
};
static const char *model_names[] = { "Haiku 4.5", "Sonnet 4.6", "Opus 4.6", NULL };
#define NUM_MODELS 3
#define DEFAULT_MODEL_IDX 1 /* Sonnet */
#define IMAGE_THUMB_SIZE 70

typedef struct {
    GdkTexture *texture;
    char *media_type;
    char *base64;
} PendingImage;

static void pending_image_free(gpointer data) {
    PendingImage *img = data;
    g_object_unref(img->texture);
    g_free(img->media_type);
    g_free(img->base64);
    g_free(img);
}

struct _QuickHelpWindow {
    GtkWindow *window;
    GtkTextView *text_view;
    GtkBox *chat_box;           /* vertical box inside scroll for messages */
    GtkScrolledWindow *scroll;
    GtkSpinner *spinner;
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
    gsize tool_status_start;    /* buf position before status text (G_MAXSIZE = none) */
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

static void on_submit(QuickHelpWindow *qh);

static void free_messages(QuickHelpWindow *qh) {
    for (int i = 0; i < qh->msg_count; i++) {
        g_free(qh->messages[i].role);
        g_free(qh->messages[i].content);
        for (int j = 0; j < qh->messages[i].image_count; j++) {
            g_free(qh->messages[i].images[j].media_type);
            g_free(qh->messages[i].images[j].base64);
        }
        g_free(qh->messages[i].images);
    }
    qh->msg_count = 0;
}

/* Returns a newly-allocated copy of the text view content (caller frees) */
static char *get_input_text(QuickHelpWindow *qh) {
    GtkTextBuffer *buf = gtk_text_view_get_buffer(qh->text_view);
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    return gtk_text_buffer_get_text(buf, &start, &end, FALSE);
}

static void set_input_text(QuickHelpWindow *qh, const char *text) {
    GtkTextBuffer *buf = gtk_text_view_get_buffer(qh->text_view);
    gtk_text_buffer_set_text(buf, text, -1);
}

static void on_destroy(GtkWidget *widget, gpointer data) {
    (void)widget;
    QuickHelpWindow *qh = data;
    free_messages(qh);
    g_free(qh->system_prompt);
    if (qh->streaming_buf)
        g_string_free(qh->streaming_buf, TRUE);
    g_free(qh->stream_error_msg);
    g_mutex_clear(&qh->stream_lock);
    g_ptr_array_unref(qh->bubble_links);
    g_ptr_array_unref(qh->pending_images);
    ai_backend_free(qh->backend);
    window_info_free(qh->info);
    system_context_free(qh->sys);
    g_free(qh);
}

static void expand_window(QuickHelpWindow *qh) {
    if (qh->expanded) return;
    qh->expanded = TRUE;
    /* Show the overlay (which contains the scroll area) */
    gtk_widget_set_visible(
        gtk_widget_get_parent(GTK_WIDGET(qh->scroll)), TRUE);
    gtk_window_set_default_size(qh->window, INITIAL_WIDTH, EXPANDED_HEIGHT);
}

/* Create a texture from base64-encoded image data */
static GdkTexture *texture_from_base64(const char *base64) {
    gsize len;
    guchar *data = g_base64_decode(base64, &len);
    GBytes *bytes = g_bytes_new_take(data, len);
    GdkTexture *tex = gdk_texture_new_from_bytes(bytes, NULL);
    g_bytes_unref(bytes);
    return tex; /* may be NULL on decode failure */
}

/* Build a horizontal scrolled row of image thumbnails */
static GtkWidget *make_image_row(AiImage *images, int count) {
    GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_scrolled_window_set_policy(sw, GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_max_content_height(sw, IMAGE_THUMB_SIZE);
    gtk_scrolled_window_set_propagate_natural_width(sw, TRUE);

    GtkBox *row = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4));
    for (int i = 0; i < count; i++) {
        GdkTexture *tex = texture_from_base64(images[i].base64);
        if (!tex) continue;
        GtkWidget *pic = gtk_picture_new_for_paintable(GDK_PAINTABLE(tex));
        gtk_widget_set_size_request(pic, IMAGE_THUMB_SIZE, IMAGE_THUMB_SIZE);
        gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_COVER);
        gtk_widget_set_overflow(pic, GTK_OVERFLOW_HIDDEN);
        gtk_box_append(row, pic);
        g_object_unref(tex);
    }
    gtk_scrolled_window_set_child(sw, GTK_WIDGET(row));
    return GTK_WIDGET(sw);
}

static gboolean is_scrolled_to_bottom(QuickHelpWindow *qh) {
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(qh->scroll);
    double val = gtk_adjustment_get_value(adj);
    double upper = gtk_adjustment_get_upper(adj);
    double page = gtk_adjustment_get_page_size(adj);
    return val >= upper - page - 1.0;
}

static void scroll_to_bottom(QuickHelpWindow *qh) {
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(qh->scroll);
    double upper = gtk_adjustment_get_upper(adj);
    double page = gtk_adjustment_get_page_size(adj);
    gtk_adjustment_set_value(adj, upper - page);
}

static void update_scroll_button(QuickHelpWindow *qh) {
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(qh->scroll);
    double val = gtk_adjustment_get_value(adj);
    gboolean at_bottom = is_scrolled_to_bottom(qh);
    /* Show chevron only when not at bottom and scroll position > 0 */
    gtk_widget_set_visible(GTK_WIDGET(qh->scroll_to_bottom),
                           val > 0 && !at_bottom);
}

static void on_scroll_value_changed(GtkAdjustment *adj, gpointer data) {
    (void)adj;
    update_scroll_button(data);
}

static void on_scroll_upper_changed(GObject *obj, GParamSpec *pspec,
                                    gpointer data) {
    (void)obj; (void)pspec;
    QuickHelpWindow *qh = data;
    if (qh->scroll_pin_bottom) {
        qh->scroll_pin_bottom = FALSE;
        qh->scroll_pin_value = -1;
        scroll_to_bottom(qh);
    } else if (qh->scroll_pin_value >= 0) {
        double val = qh->scroll_pin_value;
        qh->scroll_pin_value = -1;
        GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(qh->scroll);
        gtk_adjustment_set_value(adj, val);
    }
    update_scroll_button(qh);
}

static void on_scroll_to_bottom_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    scroll_to_bottom(data);
}

/* Helper: create a selectable, wrapping label from Pango markup */
/* Handle link clicks: open in browser, suppress GTK's built-in link focus */
static gboolean on_label_activate_link(GtkLabel *lbl, const char *uri,
                                       gpointer data) {
    (void)lbl; (void)data;
    GtkUriLauncher *launcher = gtk_uri_launcher_new(uri);
    gtk_uri_launcher_launch(launcher, NULL, NULL, NULL, NULL);
    g_object_unref(launcher);
    return TRUE; /* suppress default handling */
}

static GtkWidget *make_markup_label(const char *markup) {
    GtkLabel *lbl = GTK_LABEL(gtk_label_new(NULL));
    gtk_label_set_markup(lbl, markup);
    gtk_label_set_wrap(lbl, TRUE);
    gtk_label_set_xalign(lbl, 0.0);
    gtk_label_set_selectable(lbl, TRUE);
    g_signal_connect(lbl, "activate-link",
                     G_CALLBACK(on_label_activate_link), NULL);
    return GTK_WIDGET(lbl);
}

/* Wrap a widget in a chat bubble */
static GtkWidget *make_bubble(GtkWidget *content, gboolean is_user) {
    GtkBox *bubble = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 4));
    gtk_widget_add_css_class(GTK_WIDGET(bubble), "card");
    gtk_widget_set_margin_start(GTK_WIDGET(bubble), 8);
    gtk_widget_set_margin_end(GTK_WIDGET(bubble), 8);
    gtk_widget_set_margin_top(GTK_WIDGET(bubble), 2);
    gtk_widget_set_margin_bottom(GTK_WIDGET(bubble), 2);
    gtk_box_append(bubble, content);

    /* Outer box for alignment — small margin on the aligned side */
    GtkBox *row = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
    if (is_user)
        gtk_widget_set_margin_start(GTK_WIDGET(row), 20);
    else
        gtk_widget_set_margin_end(GTK_WIDGET(row), 20);
    if (is_user) {
        /* Right-aligned: spacer then bubble */
        GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(spacer, TRUE);
        gtk_box_append(row, spacer);
    }
    gtk_box_append(row, GTK_WIDGET(bubble));
    if (!is_user) {
        /* Left-aligned: bubble then spacer */
        GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(spacer, TRUE);
        gtk_box_append(row, spacer);
    }
    return GTK_WIDGET(row);
}

/* Render the full conversation into chat_box */
static void render_conversation(QuickHelpWindow *qh, const char *partial_assistant) {
    /* Clear chat_box */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(qh->chat_box))))
        gtk_box_remove(qh->chat_box, child);

    /* Reset per-bubble link tracking */
    g_ptr_array_set_size(qh->bubble_links, 0);
    int bubble_idx = 0;

    for (int i = 0; i < qh->msg_count; i++) {
        gboolean is_user = g_strcmp0(qh->messages[i].role, "user") == 0;
        GtkWidget *bubble;
        GPtrArray *links = g_ptr_array_new_with_free_func(g_free);

        if (is_user) {
            GtkBox *msg_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 4));
            if (qh->messages[i].image_count > 0)
                gtk_box_append(msg_box,
                    make_image_row(qh->messages[i].images,
                                   qh->messages[i].image_count));
            char *escaped = g_markup_escape_text(qh->messages[i].content, -1);
            gtk_box_append(msg_box, make_markup_label(escaped));
            g_free(escaped);
            bubble = make_bubble(GTK_WIDGET(msg_box), TRUE);
        } else {
            /* Collect links (not code blocks) for this bubble */
            GPtrArray *all_items = g_ptr_array_new_with_free_func(g_free);
            GArray *all_types = g_array_new(FALSE, FALSE, sizeof(TabItemType));
            MarkdownLinkInfo li = {
                .highlight_index = -1,
                .current_link = 0,
                .urls = all_items,
                .types = all_types
            };
            char *pango = markdown_to_pango_ex(qh->messages[i].content, &li);
            for (guint j = 0; j < all_items->len; j++) {
                if (g_array_index(all_types, TabItemType, j) == TAB_LINK)
                    g_ptr_array_add(links, g_strdup(g_ptr_array_index(all_items, j)));
            }
            g_ptr_array_unref(all_items);
            g_array_unref(all_types);
            bubble = make_bubble(make_markup_label(pango), FALSE);
            g_free(pango);
        }

        if (bubble_idx == qh->focused_bubble)
            gtk_widget_add_css_class(bubble, "focused-bubble");
        gtk_box_append(qh->chat_box, bubble);
        g_ptr_array_add(qh->bubble_links, links);
        bubble_idx++;
    }
    if (partial_assistant) {
        GPtrArray *all_items = g_ptr_array_new_with_free_func(g_free);
        GArray *all_types = g_array_new(FALSE, FALSE, sizeof(TabItemType));
        MarkdownLinkInfo li = {
            .highlight_index = -1,
            .current_link = 0,
            .urls = all_items,
            .types = all_types
        };
        char *pango = markdown_to_pango_ex(partial_assistant, &li);
        GPtrArray *links = g_ptr_array_new_with_free_func(g_free);
        for (guint j = 0; j < all_items->len; j++) {
            if (g_array_index(all_types, TabItemType, j) == TAB_LINK)
                g_ptr_array_add(links, g_strdup(g_ptr_array_index(all_items, j)));
        }
        g_ptr_array_unref(all_items);
        g_array_unref(all_types);
        GtkWidget *bubble = make_bubble(make_markup_label(pango), FALSE);
        if (bubble_idx == qh->focused_bubble)
            gtk_widget_add_css_class(bubble, "focused-bubble");
        gtk_box_append(qh->chat_box, bubble);
        g_free(pango);
        g_ptr_array_add(qh->bubble_links, links);
        bubble_idx++;
    }

    qh->bubble_count = bubble_idx;
}

/* Called on the main thread to update the UI with streaming content */
static gboolean on_stream_update(gpointer data) {
    QuickHelpWindow *qh = data;

    g_mutex_lock(&qh->stream_lock);
    qh->stream_ui_pending = FALSE;
    char *snapshot = g_strdup(qh->streaming_buf->str);
    gboolean done = !qh->streaming;
    gboolean had_error = qh->stream_had_error;
    gboolean cancelled = qh->stream_cancelled;
    char *error_msg = had_error ? g_strdup(qh->stream_error_msg) : NULL;
    g_mutex_unlock(&qh->stream_lock);

    if (done) {
        if (cancelled) {
            /* Save partial content (if any) */
            if (*snapshot && qh->msg_count < MAX_MESSAGES) {
                qh->messages[qh->msg_count].role = g_strdup("assistant");
                qh->messages[qh->msg_count].content = snapshot;
                qh->msg_count++;
                snapshot = NULL;
            }
            g_free(snapshot);
            render_conversation(qh, NULL);
        } else if (had_error) {
            g_free(snapshot);
            /* Remove the user message that triggered this error */
            if (qh->msg_count > 0) {
                qh->msg_count--;
                AiMessage *m = &qh->messages[qh->msg_count];
                set_input_text(qh, m->content);
                g_free(m->role);
                g_free(m->content);
                for (int j = 0; j < m->image_count; j++) {
                    g_free(m->images[j].media_type);
                    g_free(m->images[j].base64);
                }
                g_free(m->images);
                m->images = NULL;
                m->image_count = 0;
            }
            /* Show error in red label */
            char *markup = g_markup_printf_escaped(
                "<span foreground=\"red\">%s</span>", error_msg);
            gtk_label_set_markup(qh->error_label, markup);
            gtk_widget_set_visible(GTK_WIDGET(qh->error_label), TRUE);
            g_free(markup);
            g_free(error_msg);
            render_conversation(qh, NULL);
        } else {
            /* Finalize: store message and reset */
            if (qh->msg_count < MAX_MESSAGES) {
                qh->messages[qh->msg_count].role = g_strdup("assistant");
                qh->messages[qh->msg_count].content = snapshot;
                qh->msg_count++;
            } else {
                g_free(snapshot);
            }
            render_conversation(qh, NULL);
        }
        gtk_spinner_stop(qh->spinner);
        gtk_widget_set_visible(GTK_WIDGET(qh->spinner), FALSE);
        gtk_widget_set_visible(GTK_WIDGET(qh->stop_button), FALSE);
        gtk_widget_grab_focus(GTK_WIDGET(qh->text_view));

        /* If a submit was queued during cancel, fire it now */
        if (qh->submit_after_cancel) {
            qh->submit_after_cancel = FALSE;
            on_submit(qh);
        }
    } else {
        GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(qh->scroll);
        double prev_val = gtk_adjustment_get_value(adj);
        gboolean was_at_bottom = is_scrolled_to_bottom(qh);
        gboolean was_at_top = prev_val <= 0.0;
        /* Schedule scroll restoration for after layout recalculates upper */
        if (was_at_top)
            qh->scroll_pin_value = 0;
        else if (was_at_bottom)
            qh->scroll_pin_bottom = TRUE;
        else
            qh->scroll_pin_value = prev_val;
        render_conversation(qh, snapshot);
        g_free(snapshot);
    }

    return G_SOURCE_REMOVE;
}

/* Stream callback — called from the worker thread */
static void on_stream_chunk(const char *delta, const char *error,
                            void *user_data) {
    QuickHelpWindow *qh = user_data;

    g_mutex_lock(&qh->stream_lock);
    if (qh->stream_cancelled) {
        g_mutex_unlock(&qh->stream_lock);
        return;
    }
    /* Strip tool status indicator before appending new content */
    if (qh->tool_status_start != G_MAXSIZE) {
        g_string_truncate(qh->streaming_buf, qh->tool_status_start);
        qh->tool_status_start = G_MAXSIZE;
    }
    if (error) {
        g_string_append_printf(qh->streaming_buf, "\n\n**Error:** %s", error);
        qh->stream_had_error = TRUE;
        g_free(qh->stream_error_msg);
        qh->stream_error_msg = g_strdup(error);
        qh->streaming = FALSE;
    } else if (delta) {
        g_string_append(qh->streaming_buf, delta);
    } else {
        /* NULL delta + NULL error = done */
        qh->streaming = FALSE;
    }
    if (!qh->stream_ui_pending) {
        qh->stream_ui_pending = TRUE;
        g_idle_add(on_stream_update, qh);
    }
    g_mutex_unlock(&qh->stream_lock);
}

/* Tool status callback — called from the worker thread */
static void on_tool_status(const char *tool_name, const char *detail,
                           void *user_data) {
    (void)tool_name;
    QuickHelpWindow *qh = user_data;

    g_mutex_lock(&qh->stream_lock);
    /* Remove any previous status line first */
    if (qh->tool_status_start != G_MAXSIZE)
        g_string_truncate(qh->streaming_buf, qh->tool_status_start);
    /* Record position, then append status */
    qh->tool_status_start = qh->streaming_buf->len;
    char *status = g_strdup_printf("\n\n*Fetching %s\u2026*", detail);
    g_string_append(qh->streaming_buf, status);
    g_free(status);
    if (!qh->stream_ui_pending) {
        qh->stream_ui_pending = TRUE;
        g_idle_add(on_stream_update, qh);
    }
    g_mutex_unlock(&qh->stream_lock);
}

static gpointer send_thread(gpointer data) {
    QuickHelpWindow *qh = data;

    qh->backend->send_stream(
        qh->backend, qh->system_prompt,
        qh->messages, qh->msg_count,
        on_stream_chunk, qh
    );

    return NULL;
}

static void cancel_stream(QuickHelpWindow *qh) {
    g_mutex_lock(&qh->stream_lock);
    if (!qh->streaming) {
        g_mutex_unlock(&qh->stream_lock);
        return;
    }
    /* Strip any tool status indicator */
    if (qh->tool_status_start != G_MAXSIZE) {
        g_string_truncate(qh->streaming_buf, qh->tool_status_start);
        qh->tool_status_start = G_MAXSIZE;
    }
    qh->streaming = FALSE;
    qh->stream_cancelled = TRUE;
    if (!qh->stream_ui_pending) {
        qh->stream_ui_pending = TRUE;
        g_idle_add(on_stream_update, qh);
    }
    g_mutex_unlock(&qh->stream_lock);
    /* Signal curl to abort */
    g_atomic_int_set(&qh->backend->cancel_requested, 1);
}

static void on_stop_clicked(GtkButton *button, gpointer data) {
    (void)button;
    cancel_stream(data);
}

/* Rebuild the image preview thumbnails from pending_images */
static void rebuild_image_preview(QuickHelpWindow *qh) {
    /* Remove all children */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(qh->image_preview_box))))
        gtk_box_remove(qh->image_preview_box, child);

    for (guint i = 0; i < qh->pending_images->len; i++) {
        PendingImage *img = g_ptr_array_index(qh->pending_images, i);
        GtkWidget *picture = gtk_picture_new_for_paintable(
            GDK_PAINTABLE(img->texture));
        gtk_widget_set_size_request(picture, IMAGE_THUMB_SIZE, IMAGE_THUMB_SIZE);
        gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_COVER);
        gtk_widget_set_overflow(picture, GTK_OVERFLOW_HIDDEN);
        gtk_box_append(qh->image_preview_box, picture);
    }

    gtk_widget_set_visible(GTK_WIDGET(qh->image_preview_box),
                           qh->pending_images->len > 0);
}

static void add_pending_image(QuickHelpWindow *qh, GdkTexture *texture,
                              const char *media_type, char *base64) {
    PendingImage *img = g_new0(PendingImage, 1);
    img->texture = g_object_ref(texture);
    img->media_type = g_strdup(media_type);
    img->base64 = base64; /* takes ownership */
    g_ptr_array_add(qh->pending_images, img);
    rebuild_image_preview(qh);
}

/* Clipboard image read callback */
static void on_clipboard_image_ready(GObject *source, GAsyncResult *res,
                                     gpointer data) {
    QuickHelpWindow *qh = data;
    GdkTexture *texture = gdk_clipboard_read_texture_finish(
        GDK_CLIPBOARD(source), res, NULL);
    if (!texture) return;

    GBytes *png = gdk_texture_save_to_png_bytes(texture);
    char *base64 = g_base64_encode(g_bytes_get_data(png, NULL),
                                   g_bytes_get_size(png));
    g_bytes_unref(png);

    add_pending_image(qh, texture, "image/png", base64);
    g_object_unref(texture);
}

static const char *media_type_for_path(const char *path) {
    if (g_str_has_suffix(path, ".png")) return "image/png";
    if (g_str_has_suffix(path, ".jpg") || g_str_has_suffix(path, ".jpeg"))
        return "image/jpeg";
    if (g_str_has_suffix(path, ".gif")) return "image/gif";
    if (g_str_has_suffix(path, ".webp")) return "image/webp";
    return NULL;
}

static void add_image_from_file(QuickHelpWindow *qh, GFile *file) {
    char *path = g_file_get_path(file);
    if (!path) return;

    const char *media_type = media_type_for_path(path);
    if (!media_type) { g_free(path); return; }

    gsize len;
    char *data;
    if (!g_file_get_contents(path, &data, &len, NULL)) {
        g_free(path);
        return;
    }

    char *base64 = g_base64_encode((const guchar *)data, len);
    g_free(data);

    GdkTexture *texture = gdk_texture_new_from_file(file, NULL);
    if (!texture) { g_free(base64); g_free(path); return; }

    add_pending_image(qh, texture, media_type, base64);
    g_object_unref(texture);
    g_free(path);
}

/* Drag & drop handlers */
static GdkDragAction on_drop_enter(GtkDropTarget *target, double x, double y,
                                   gpointer data) {
    (void)target; (void)x; (void)y;
    QuickHelpWindow *qh = data;
    gtk_widget_set_visible(qh->drop_overlay, TRUE);
    return GDK_ACTION_COPY;
}

static void on_drop_leave(GtkDropTarget *target, gpointer data) {
    (void)target;
    QuickHelpWindow *qh = data;
    gtk_widget_set_visible(qh->drop_overlay, FALSE);
}

static gboolean on_drop(GtkDropTarget *target, const GValue *value,
                        double x, double y, gpointer data) {
    (void)target; (void)x; (void)y;
    QuickHelpWindow *qh = data;
    gtk_widget_set_visible(qh->drop_overlay, FALSE);

    if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
        GSList *files = gdk_file_list_get_files(g_value_get_boxed(value));
        for (GSList *l = files; l; l = l->next)
            add_image_from_file(qh, G_FILE(l->data));
        return TRUE;
    }
    return FALSE;
}

static void on_submit(QuickHelpWindow *qh) {
    char *text = get_input_text(qh);
    g_strstrip(text);
    gboolean has_images = qh->pending_images->len > 0;
    if (!*text && !has_images) { g_free(text); return; }
    if (!*text) {
        /* Images with no text: provide a default prompt */
        g_free(text);
        text = g_strdup("What's in this image?");
    }

    qh->focused_bubble = -1;
    gtk_widget_set_visible(GTK_WIDGET(qh->error_label), FALSE);
    expand_window(qh);

    /* Append user message */
    if (qh->msg_count >= MAX_MESSAGES) { g_free(text); return; }
    qh->messages[qh->msg_count].role = g_strdup("user");
    qh->messages[qh->msg_count].content = text; /* transfer ownership */

    /* Transfer pending images to message */
    if (has_images) {
        int n = qh->pending_images->len;
        qh->messages[qh->msg_count].images = g_new(AiImage, n);
        qh->messages[qh->msg_count].image_count = n;
        for (int i = 0; i < n; i++) {
            PendingImage *pi = g_ptr_array_index(qh->pending_images, i);
            qh->messages[qh->msg_count].images[i].media_type =
                g_strdup(pi->media_type);
            qh->messages[qh->msg_count].images[i].base64 =
                g_strdup(pi->base64);
        }
        g_ptr_array_set_size(qh->pending_images, 0);
        rebuild_image_preview(qh);
    }
    qh->msg_count++;

    /* Clear input */
    set_input_text(qh, "");
    gtk_widget_set_visible(GTK_WIDGET(qh->spinner), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(qh->stop_button), TRUE);
    gtk_spinner_start(qh->spinner);

    /* Show conversation with "Thinking..." */
    qh->scroll_pin_bottom = TRUE;
    render_conversation(qh, NULL);

    /* Prepare streaming state */
    g_mutex_lock(&qh->stream_lock);
    if (qh->streaming_buf)
        g_string_truncate(qh->streaming_buf, 0);
    else
        qh->streaming_buf = g_string_new(NULL);
    qh->streaming = TRUE;
    qh->stream_had_error = FALSE;
    qh->stream_cancelled = FALSE;
    g_free(qh->stream_error_msg);
    qh->stream_error_msg = NULL;
    qh->stream_ui_pending = FALSE;
    qh->tool_status_start = G_MAXSIZE;
    g_mutex_unlock(&qh->stream_lock);

    /* Send in background thread */
    g_thread_unref(g_thread_new("ai-stream", send_thread, qh));
}

static void on_model_changed(GObject *obj, GParamSpec *pspec, gpointer data) {
    (void)pspec;
    QuickHelpWindow *qh = data;
    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    if (idx < NUM_MODELS) {
        g_free(qh->backend->model);
        qh->backend->model = g_strdup(model_ids[idx]);
    }
    gtk_widget_grab_focus(GTK_WIDGET(qh->text_view));
}

/* Callback when a link is clicked in the link picker popover */
static void on_link_picker_clicked(GtkButton *btn, gpointer data) {
    GtkPopover *popover = GTK_POPOVER(data);
    const char *url = g_object_get_data(G_OBJECT(btn), "url");
    GtkWidget *win = gtk_widget_get_ancestor(GTK_WIDGET(popover), GTK_TYPE_WINDOW);
    GtkUriLauncher *launcher = gtk_uri_launcher_new(url);
    gtk_uri_launcher_launch(launcher, GTK_WINDOW(win), NULL, NULL, NULL);
    g_object_unref(launcher);
    gtk_popover_popdown(popover);
}

/* Show a popover with links from the focused bubble */
static void show_link_picker(QuickHelpWindow *qh, GPtrArray *links) {
    /* Find the focused bubble widget in chat_box */
    GtkWidget *target = gtk_widget_get_first_child(GTK_WIDGET(qh->chat_box));
    for (int i = 0; i < qh->focused_bubble && target; i++)
        target = gtk_widget_get_next_sibling(target);
    if (!target) return;

    GtkWidget *popover = gtk_popover_new();
    gtk_widget_set_parent(popover, target);

    GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 2));
    for (guint i = 0; i < links->len; i++) {
        const char *url = g_ptr_array_index(links, i);
        GtkWidget *btn = gtk_button_new_with_label(url);
        gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
        gtk_widget_set_halign(btn, GTK_ALIGN_START);
        g_object_set_data_full(G_OBJECT(btn), "url", g_strdup(url), g_free);
        g_signal_connect(btn, "clicked",
                         G_CALLBACK(on_link_picker_clicked), popover);
        gtk_box_append(box, btn);
    }
    gtk_popover_set_child(GTK_POPOVER(popover), GTK_WIDGET(box));
    gtk_popover_popup(GTK_POPOVER(popover));
}

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer data) {
    (void)ctrl; (void)keycode;
    QuickHelpWindow *qh = data;

    /* Let the model dropdown (and its popover) handle its own keys */
    GtkWidget *focus = gtk_window_get_focus(qh->window);
    if (focus && gtk_widget_is_ancestor(focus, GTK_WIDGET(qh->model_dropdown)))
        return FALSE;

    /* Ctrl+Escape: cancel streaming */
    if (keyval == GDK_KEY_Escape && (state & GDK_CONTROL_MASK)) {
        if (qh->streaming)
            cancel_stream(qh);
        return TRUE;
    }

    /* Enter = submit, Shift+Enter = newline */
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        if (state & GDK_SHIFT_MASK)
            return FALSE; /* let GtkTextView insert newline */

        if (qh->streaming) {
            /* Cancel current stream; submit will fire when it finishes */
            char *text = get_input_text(qh);
            g_strstrip(text);
            if (*text || qh->pending_images->len > 0) {
                qh->submit_after_cancel = TRUE;
                cancel_stream(qh);
            }
            g_free(text);
            return TRUE;
        }

        char *text = get_input_text(qh);
        g_strstrip(text);
        gboolean empty = !*text;
        g_free(text);

        /* If input is empty and a bubble is focused, open its links */
        if (empty && qh->focused_bubble >= 0 &&
            qh->focused_bubble < (int)qh->bubble_links->len) {
            GPtrArray *links = g_ptr_array_index(qh->bubble_links,
                                                  qh->focused_bubble);
            if (links->len == 1) {
                const char *url = g_ptr_array_index(links, 0);
                GtkUriLauncher *launcher = gtk_uri_launcher_new(url);
                gtk_uri_launcher_launch(launcher, qh->window, NULL, NULL, NULL);
                g_object_unref(launcher);
            } else if (links->len > 1) {
                show_link_picker(qh, links);
            }
            return TRUE;
        }

        on_submit(qh);
        return TRUE;
    }

    /* Ctrl+V: check for image in clipboard before text paste */
    if (keyval == GDK_KEY_v && (state & GDK_CONTROL_MASK)) {
        GdkClipboard *clip = gdk_display_get_clipboard(
            gdk_display_get_default());
        GdkContentFormats *formats = gdk_clipboard_get_formats(clip);
        if (gdk_content_formats_contain_gtype(formats, GDK_TYPE_TEXTURE)) {
            gdk_clipboard_read_texture_async(clip, NULL,
                on_clipboard_image_ready, qh);
            return TRUE;
        }
        return FALSE; /* let text view handle text paste */
    }

    if (keyval == GDK_KEY_Escape) {
        gtk_window_close(qh->window);
        return TRUE;
    }

    if (keyval == GDK_KEY_n && (state & GDK_CONTROL_MASK)) {
        free_messages(qh);
        g_mutex_lock(&qh->stream_lock);
        if (qh->streaming_buf)
            g_string_truncate(qh->streaming_buf, 0);
        g_mutex_unlock(&qh->stream_lock);
        /* Clear chat_box */
        {
            GtkWidget *c;
            while ((c = gtk_widget_get_first_child(GTK_WIDGET(qh->chat_box))))
                gtk_box_remove(qh->chat_box, c);
        }
        gtk_widget_set_visible(GTK_WIDGET(qh->error_label), FALSE);
        g_ptr_array_set_size(qh->bubble_links, 0);
        qh->focused_bubble = -1;
        qh->bubble_count = 0;
        gtk_widget_set_visible(
            gtk_widget_get_parent(GTK_WIDGET(qh->scroll)), FALSE);
        qh->expanded = FALSE;
        gtk_window_set_default_size(qh->window, INITIAL_WIDTH, INITIAL_HEIGHT);
        set_input_text(qh, "");
        g_ptr_array_set_size(qh->pending_images, 0);
        rebuild_image_preview(qh);
        gtk_widget_grab_focus(GTK_WIDGET(qh->text_view));
        return TRUE;
    }

    /* Tab / Shift+Tab: cycle through assistant bubbles */
    if ((keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab) &&
        qh->bubble_count > 0 && !qh->streaming) {
        if (keyval == GDK_KEY_ISO_Left_Tab || (state & GDK_SHIFT_MASK)) {
            qh->focused_bubble--;
            if (qh->focused_bubble < -1)
                qh->focused_bubble = qh->bubble_count - 1;
        } else {
            qh->focused_bubble++;
            if (qh->focused_bubble >= qh->bubble_count)
                qh->focused_bubble = -1;
        }
        render_conversation(qh, NULL);
        return TRUE;
    }

    /* Ctrl+Alt+Up/Down: jump to prev/next user message */
    if ((keyval == GDK_KEY_Up || keyval == GDK_KEY_Down) &&
        (state & GDK_CONTROL_MASK) && (state & GDK_ALT_MASK)) {
        /* Find the user message widgets in chat_box */
        GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(qh->chat_box));
        GPtrArray *user_widgets = g_ptr_array_new();
        for (int i = 0; i < qh->msg_count && child; i++) {
            if (g_strcmp0(qh->messages[i].role, "user") == 0)
                g_ptr_array_add(user_widgets, child);
            child = gtk_widget_get_next_sibling(child);
        }
        if (user_widgets->len > 0) {
            GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(qh->scroll);
            double cur = gtk_adjustment_get_value(adj);
            GtkWidget *target = NULL;
            graphene_point_t p = GRAPHENE_POINT_INIT(0, 0);
            graphene_point_t out;

            if (keyval == GDK_KEY_Down) {
                for (guint i = 0; i < user_widgets->len; i++) {
                    GtkWidget *w = g_ptr_array_index(user_widgets, i);
                    if (gtk_widget_compute_point(w, GTK_WIDGET(qh->chat_box),
                                                 &p, &out) &&
                        out.y > cur + 1.0) { target = w; break; }
                }
            } else {
                for (int i = user_widgets->len - 1; i >= 0; i--) {
                    GtkWidget *w = g_ptr_array_index(user_widgets, i);
                    if (gtk_widget_compute_point(w, GTK_WIDGET(qh->chat_box),
                                                 &p, &out) &&
                        out.y < cur - 1.0) { target = w; break; }
                }
            }
            if (target &&
                gtk_widget_compute_point(target, GTK_WIDGET(qh->chat_box),
                                         &p, &out))
                gtk_adjustment_set_value(adj, out.y);
        }
        g_ptr_array_unref(user_widgets);
        return TRUE;
    }

    /* Alt+Up/Down: cycle model */
    if ((keyval == GDK_KEY_Up || keyval == GDK_KEY_Down) && (state & GDK_ALT_MASK)) {
        guint idx = gtk_drop_down_get_selected(qh->model_dropdown);
        if (keyval == GDK_KEY_Up && idx > 0)
            gtk_drop_down_set_selected(qh->model_dropdown, idx - 1);
        else if (keyval == GDK_KEY_Down && idx < NUM_MODELS - 1)
            gtk_drop_down_set_selected(qh->model_dropdown, idx + 1);
        return TRUE;
    }

    /* Ctrl+Home/End: scroll to top/bottom */
    if ((keyval == GDK_KEY_Home || keyval == GDK_KEY_End) &&
        (state & GDK_CONTROL_MASK)) {
        GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(qh->scroll);
        if (keyval == GDK_KEY_Home)
            gtk_adjustment_set_value(adj, gtk_adjustment_get_lower(adj));
        else
            scroll_to_bottom(qh);
        return TRUE;
    }

    if ((keyval == GDK_KEY_Up || keyval == GDK_KEY_Down ||
         keyval == GDK_KEY_Page_Up || keyval == GDK_KEY_Page_Down) &&
        (state & GDK_CONTROL_MASK)) {
        GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(qh->scroll);
        double val = gtk_adjustment_get_value(adj);
        double step = (keyval == GDK_KEY_Page_Up || keyval == GDK_KEY_Page_Down)
            ? gtk_adjustment_get_page_size(adj)
            : SCROLL_STEP;
        if (keyval == GDK_KEY_Up || keyval == GDK_KEY_Page_Up)
            val -= step;
        else
            val += step;
        double lower = gtk_adjustment_get_lower(adj);
        double upper = gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj);
        if (val < lower) val = lower;
        if (val > upper) val = upper;
        gtk_adjustment_set_value(adj, val);
        return TRUE;
    }

    return FALSE;
}

QuickHelpWindow *quick_help_window_new(GtkApplication *app,
                                        AiBackend *backend,
                                        WindowInfo *info,
                                        SystemContext *sys,
                                        gboolean hide_decorations,
                                        const char *default_model) {
    QuickHelpWindow *qh = g_new0(QuickHelpWindow, 1);
    qh->backend = backend;
    qh->info = info;
    qh->sys = sys;
    g_mutex_init(&qh->stream_lock);
    qh->tool_status_start = G_MAXSIZE;
    qh->focused_bubble = -1;
    qh->scroll_pin_value = -1;
    qh->bubble_links = g_ptr_array_new_with_free_func(
        (GDestroyNotify)g_ptr_array_unref);
    qh->pending_images = g_ptr_array_new_with_free_func(pending_image_free);

    /* Wire up tool status notifications */
    backend->tool_status_cb = on_tool_status;
    backend->tool_status_data = qh;

    /* Build system prompt */
    GString *prompt = g_string_new(
        "You are a helpful assistant providing quick help for desktop applications. ");

    /* System context */
    if (sys) {
        g_string_append_printf(prompt, "The user is running %s", sys->os_name);
        if (sys->desktop_env)
            g_string_append_printf(prompt, " with the %s desktop", sys->desktop_env);
        if (sys->display_server)
            g_string_append_printf(prompt, " on %s", sys->display_server);
        g_string_append(prompt, ". ");
        if (sys->shell)
            g_string_append_printf(prompt,
                "Their default shell is %s. ", sys->shell);
    }

    /* Window context */
    if (info) {
        g_string_append_printf(prompt,
            "The user is currently working in **%s** (window title: \"%s\"). ",
            info->app_name, info->title);
    }

    g_string_append(prompt,
        "If the question is about the focused application, give concise, actionable answers: "
        "prioritize keyboard shortcuts first, then menu navigation, and keep it brief. "
        "For general questions unrelated to the app, answer normally without artificially constraining length. "
        "Use markdown formatting: bold for emphasis, backticks for keyboard shortcuts and code.");

    qh->system_prompt = g_string_free(prompt, FALSE);

    /* Load CSS for chat bubbles */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        ".card { padding: 8px 12px; border-radius: 12px; "
        "background: alpha(currentColor, 0.08); }"
        ".focused-bubble .card { outline: 2px solid @accent_color; }");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    /* Create window */
    qh->window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(qh->window, "Quick Help");
    gtk_window_set_default_size(qh->window, INITIAL_WIDTH, INITIAL_HEIGHT);
    gtk_window_set_resizable(qh->window, TRUE);
    if (hide_decorations)
        gtk_window_set_decorated(qh->window, FALSE);

    /* Escape and scroll key handler (capture phase so GtkText doesn't
       partially handle Tab/arrows before we suppress them) */
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_ctrl, GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_key_pressed), qh);
    gtk_widget_add_controller(GTK_WIDGET(qh->window), key_ctrl);

    /* Main vertical box inside a top-level overlay (for drop indicator) */
    GtkOverlay *window_overlay = GTK_OVERLAY(gtk_overlay_new());
    gtk_window_set_child(qh->window, GTK_WIDGET(window_overlay));

    qh->vbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 6));
    gtk_widget_set_margin_start(GTK_WIDGET(qh->vbox), 8);
    gtk_widget_set_margin_end(GTK_WIDGET(qh->vbox), 8);
    gtk_widget_set_margin_top(GTK_WIDGET(qh->vbox), 8);
    gtk_widget_set_margin_bottom(GTK_WIDGET(qh->vbox), 8);
    gtk_overlay_set_child(window_overlay, GTK_WIDGET(qh->vbox));

    /* Drop overlay label */
    qh->drop_overlay = gtk_label_new("Drop images here");
    gtk_widget_add_css_class(qh->drop_overlay, "osd");
    gtk_widget_set_halign(qh->drop_overlay, GTK_ALIGN_FILL);
    gtk_widget_set_valign(qh->drop_overlay, GTK_ALIGN_FILL);
    gtk_widget_set_visible(qh->drop_overlay, FALSE);
    gtk_overlay_add_overlay(window_overlay, qh->drop_overlay);

    /* Input row: entry + spinner */
    GtkBox *input_row = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6));
    gtk_box_append(qh->vbox, GTK_WIDGET(input_row));

    GtkScrolledWindow *input_scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_scrolled_window_set_policy(input_scroll, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(input_scroll, 50);
    gtk_scrolled_window_set_propagate_natural_height(input_scroll, TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(input_scroll), TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(input_scroll), "frame");

    qh->text_view = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_wrap_mode(qh->text_view, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_accepts_tab(qh->text_view, FALSE);
    gtk_scrolled_window_set_child(input_scroll, GTK_WIDGET(qh->text_view));
    gtk_box_append(input_row, GTK_WIDGET(input_scroll));

    qh->spinner = GTK_SPINNER(gtk_spinner_new());
    gtk_widget_set_visible(GTK_WIDGET(qh->spinner), FALSE);
    gtk_box_append(input_row, GTK_WIDGET(qh->spinner));

    qh->stop_button = GTK_BUTTON(gtk_button_new_from_icon_name("media-playback-stop-symbolic"));
    gtk_widget_set_visible(GTK_WIDGET(qh->stop_button), FALSE);
    g_signal_connect(qh->stop_button, "clicked",
                     G_CALLBACK(on_stop_clicked), qh);
    gtk_box_append(input_row, GTK_WIDGET(qh->stop_button));

    /* Model selector dropdown */
    GtkStringList *model_list = gtk_string_list_new(model_names);
    qh->model_dropdown = GTK_DROP_DOWN(gtk_drop_down_new(
        G_LIST_MODEL(model_list), NULL));
    /* Find default model index */
    guint default_idx = DEFAULT_MODEL_IDX;
    if (default_model) {
        for (guint i = 0; i < NUM_MODELS; i++) {
            if (strcmp(default_model, model_ids[i]) == 0) {
                default_idx = i;
                break;
            }
        }
    }
    gtk_drop_down_set_selected(qh->model_dropdown, default_idx);
    g_free(qh->backend->model);
    qh->backend->model = g_strdup(model_ids[default_idx]);
    gtk_widget_set_size_request(GTK_WIDGET(qh->model_dropdown), 120, -1);
    gtk_widget_set_valign(GTK_WIDGET(qh->model_dropdown), GTK_ALIGN_START);
    g_signal_connect(qh->model_dropdown, "notify::selected",
                     G_CALLBACK(on_model_changed), qh);
    gtk_box_append(input_row, GTK_WIDGET(qh->model_dropdown));

    /* Image preview bar (hidden until images are attached) */
    qh->image_preview_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4));
    gtk_widget_set_visible(GTK_WIDGET(qh->image_preview_box), FALSE);
    gtk_box_append(qh->vbox, GTK_WIDGET(qh->image_preview_box));

    /* Drag & drop target for image files */
    GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST,
                                              GDK_ACTION_COPY);
    g_signal_connect(drop, "enter", G_CALLBACK(on_drop_enter), qh);
    g_signal_connect(drop, "leave", G_CALLBACK(on_drop_leave), qh);
    g_signal_connect(drop, "drop", G_CALLBACK(on_drop), qh);
    gtk_widget_add_controller(GTK_WIDGET(qh->window),
                              GTK_EVENT_CONTROLLER(drop));

    /* Error label (hidden until an error occurs) */
    qh->error_label = GTK_LABEL(gtk_label_new(NULL));
    gtk_label_set_wrap(qh->error_label, TRUE);
    gtk_label_set_xalign(qh->error_label, 0.0);
    gtk_widget_set_visible(GTK_WIDGET(qh->error_label), FALSE);
    gtk_box_append(qh->vbox, GTK_WIDGET(qh->error_label));

    /* Scrolled response area (hidden initially), with overlay for chevron */
    GtkOverlay *overlay = GTK_OVERLAY(gtk_overlay_new());
    gtk_widget_set_vexpand(GTK_WIDGET(overlay), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(overlay), FALSE);
    gtk_box_append(qh->vbox, GTK_WIDGET(overlay));

    qh->scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_scrolled_window_set_policy(qh->scroll, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_overlay_set_child(overlay, GTK_WIDGET(qh->scroll));

    /* Floating scroll-to-bottom button */
    qh->scroll_to_bottom = GTK_BUTTON(gtk_button_new_from_icon_name(
        "go-down-symbolic"));
    gtk_widget_add_css_class(GTK_WIDGET(qh->scroll_to_bottom), "circular");
    gtk_widget_add_css_class(GTK_WIDGET(qh->scroll_to_bottom), "osd");
    gtk_widget_set_halign(GTK_WIDGET(qh->scroll_to_bottom), GTK_ALIGN_CENTER);
    gtk_widget_set_valign(GTK_WIDGET(qh->scroll_to_bottom), GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(GTK_WIDGET(qh->scroll_to_bottom), 8);
    gtk_widget_set_visible(GTK_WIDGET(qh->scroll_to_bottom), FALSE);
    g_signal_connect(qh->scroll_to_bottom, "clicked",
                     G_CALLBACK(on_scroll_to_bottom_clicked), qh);
    gtk_overlay_add_overlay(overlay, GTK_WIDGET(qh->scroll_to_bottom));

    /* Track scroll position to show/hide chevron */
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(qh->scroll);
    g_signal_connect(vadj, "value-changed",
                     G_CALLBACK(on_scroll_value_changed), qh);
    g_signal_connect(vadj, "notify::upper",
                     G_CALLBACK(on_scroll_upper_changed), qh);

    qh->chat_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 8));
    gtk_widget_set_margin_top(GTK_WIDGET(qh->chat_box), 4);
    gtk_widget_set_margin_bottom(GTK_WIDGET(qh->chat_box), 4);
    gtk_scrolled_window_set_child(qh->scroll, GTK_WIDGET(qh->chat_box));

    g_signal_connect(qh->window, "destroy", G_CALLBACK(on_destroy), qh);

    gtk_window_present(qh->window);
    gtk_widget_grab_focus(GTK_WIDGET(qh->text_view));

    return qh;
}
