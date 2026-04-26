#include "window.h"
#include "system_context.h"
#include "markdown.h"
#include <string.h>

#define INITIAL_WIDTH  500
#define INITIAL_HEIGHT 60
#define EXPANDED_HEIGHT 500
#define MAX_MESSAGES 50
#define SCROLL_STEP 60.0

struct _QuickHelpWindow {
    GtkWindow *window;
    GtkEntry *entry;
    GtkLabel *response_label;
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
    int focused_link;        /* currently focused link index (-1 = none) */
    int link_count;          /* total links in last render */
    GPtrArray *link_urls;    /* unescaped URLs from last render */
    GtkLabel *error_label;   /* red error message below entry */
    gboolean stream_had_error; /* protected by stream_lock */
    char *stream_error_msg;    /* protected by stream_lock */
};

static void free_messages(QuickHelpWindow *qh) {
    for (int i = 0; i < qh->msg_count; i++) {
        g_free(qh->messages[i].role);
        g_free(qh->messages[i].content);
    }
    qh->msg_count = 0;
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
    g_ptr_array_unref(qh->link_urls);
    ai_backend_free(qh->backend);
    window_info_free(qh->info);
    system_context_free(qh->sys);
    g_free(qh);
}

static void expand_window(QuickHelpWindow *qh) {
    if (qh->expanded) return;
    qh->expanded = TRUE;
    gtk_widget_set_visible(GTK_WIDGET(qh->scroll), TRUE);
    gtk_window_set_default_size(qh->window, INITIAL_WIDTH, EXPANDED_HEIGHT);
}

/* Render the full conversation to the label */
static void render_conversation(QuickHelpWindow *qh, const char *partial_assistant) {
    GString *display = g_string_new(NULL);

    /* Set up link tracking */
    g_ptr_array_set_size(qh->link_urls, 0);
    MarkdownLinkInfo li = {
        .highlight_index = qh->focused_link,
        .current_link = 0,
        .urls = qh->link_urls
    };

    for (int i = 0; i < qh->msg_count; i++) {
        if (g_strcmp0(qh->messages[i].role, "user") == 0) {
            char *escaped = g_markup_escape_text(qh->messages[i].content, -1);
            g_string_append_printf(display, "<b>You:</b> %s\n\n", escaped);
            g_free(escaped);
        } else {
            char *pango = markdown_to_pango_ex(qh->messages[i].content, &li);
            g_string_append_printf(display, "%s\n\n", pango);
            g_free(pango);
        }
    }
    if (partial_assistant) {
        char *pango = markdown_to_pango_ex(partial_assistant, &li);
        g_string_append_printf(display, "%s", pango);
        g_free(pango);
    }

    qh->link_count = li.current_link;

    if (display->len > 2 && !partial_assistant)
        g_string_truncate(display, display->len - 2);
    gtk_label_set_markup(qh->response_label, display->str);
    g_string_free(display, TRUE);
}

/* Called on the main thread to update the UI with streaming content */
static gboolean on_stream_update(gpointer data) {
    QuickHelpWindow *qh = data;

    g_mutex_lock(&qh->stream_lock);
    qh->stream_ui_pending = FALSE;
    char *snapshot = g_strdup(qh->streaming_buf->str);
    gboolean done = !qh->streaming;
    gboolean had_error = qh->stream_had_error;
    char *error_msg = had_error ? g_strdup(qh->stream_error_msg) : NULL;
    g_mutex_unlock(&qh->stream_lock);

    if (done) {
        if (had_error) {
            g_free(snapshot);
            /* Remove the user message that triggered this error */
            if (qh->msg_count > 0) {
                qh->msg_count--;
                gtk_editable_set_text(GTK_EDITABLE(qh->entry),
                                      qh->messages[qh->msg_count].content);
                g_free(qh->messages[qh->msg_count].role);
                g_free(qh->messages[qh->msg_count].content);
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
        gtk_widget_set_sensitive(GTK_WIDGET(qh->entry), TRUE);
        gtk_widget_grab_focus(GTK_WIDGET(qh->entry));
    } else {
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

static void on_submit(QuickHelpWindow *qh) {
    const char *text = gtk_editable_get_text(GTK_EDITABLE(qh->entry));
    if (!text || !*text) return;

    qh->focused_link = -1;
    gtk_widget_set_visible(GTK_WIDGET(qh->error_label), FALSE);
    expand_window(qh);

    /* Append user message */
    if (qh->msg_count >= MAX_MESSAGES) return;
    qh->messages[qh->msg_count].role = g_strdup("user");
    qh->messages[qh->msg_count].content = g_strdup(text);
    qh->msg_count++;

    /* Clear entry and show spinner */
    gtk_editable_set_text(GTK_EDITABLE(qh->entry), "");
    gtk_widget_set_sensitive(GTK_WIDGET(qh->entry), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(qh->spinner), TRUE);
    gtk_spinner_start(qh->spinner);

    /* Show conversation with "Thinking..." */
    render_conversation(qh, NULL);

    /* Prepare streaming state */
    g_mutex_lock(&qh->stream_lock);
    if (qh->streaming_buf)
        g_string_truncate(qh->streaming_buf, 0);
    else
        qh->streaming_buf = g_string_new(NULL);
    qh->streaming = TRUE;
    qh->stream_had_error = FALSE;
    g_free(qh->stream_error_msg);
    qh->stream_error_msg = NULL;
    qh->stream_ui_pending = FALSE;
    qh->tool_status_start = G_MAXSIZE;
    g_mutex_unlock(&qh->stream_lock);

    /* Send in background thread */
    g_thread_unref(g_thread_new("ai-stream", send_thread, qh));
}

static void on_entry_activate(GtkEntry *entry, gpointer data) {
    (void)entry;
    QuickHelpWindow *qh = data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(qh->entry));

    /* If entry is empty and a link is focused, open the link */
    if ((!text || !*text) && qh->focused_link >= 0 &&
        qh->focused_link < (int)qh->link_urls->len) {
        const char *url = g_ptr_array_index(qh->link_urls, qh->focused_link);
        GtkUriLauncher *launcher = gtk_uri_launcher_new(url);
        gtk_uri_launcher_launch(launcher, qh->window, NULL, NULL, NULL);
        g_object_unref(launcher);
        return;
    }

    on_submit(qh);
}

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer data) {
    (void)ctrl; (void)keycode; (void)state;
    QuickHelpWindow *qh = data;

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
        gtk_label_set_markup(qh->response_label, "");
        gtk_widget_set_visible(GTK_WIDGET(qh->error_label), FALSE);
        g_ptr_array_set_size(qh->link_urls, 0);
        qh->focused_link = -1;
        qh->link_count = 0;
        gtk_widget_set_visible(GTK_WIDGET(qh->scroll), FALSE);
        qh->expanded = FALSE;
        gtk_window_set_default_size(qh->window, INITIAL_WIDTH, INITIAL_HEIGHT);
        gtk_editable_set_text(GTK_EDITABLE(qh->entry), "");
        gtk_widget_grab_focus(GTK_WIDGET(qh->entry));
        return TRUE;
    }

    /* Tab / Shift+Tab: cycle through links */
    if ((keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab) &&
        qh->link_count > 0 && !qh->streaming) {
        if (keyval == GDK_KEY_ISO_Left_Tab || (state & GDK_SHIFT_MASK)) {
            qh->focused_link--;
            if (qh->focused_link < -1)
                qh->focused_link = qh->link_count - 1;
        } else {
            qh->focused_link++;
            if (qh->focused_link >= qh->link_count)
                qh->focused_link = -1;
        }
        render_conversation(qh, NULL);
        return TRUE;
    }

    if (keyval == GDK_KEY_Up || keyval == GDK_KEY_Down ||
        keyval == GDK_KEY_Page_Up || keyval == GDK_KEY_Page_Down) {
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
                                        gboolean hide_decorations) {
    QuickHelpWindow *qh = g_new0(QuickHelpWindow, 1);
    qh->backend = backend;
    qh->info = info;
    qh->sys = sys;
    g_mutex_init(&qh->stream_lock);
    qh->tool_status_start = G_MAXSIZE;
    qh->focused_link = -1;
    qh->link_urls = g_ptr_array_new_with_free_func(g_free);

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

    /* Main vertical box */
    qh->vbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 6));
    gtk_widget_set_margin_start(GTK_WIDGET(qh->vbox), 8);
    gtk_widget_set_margin_end(GTK_WIDGET(qh->vbox), 8);
    gtk_widget_set_margin_top(GTK_WIDGET(qh->vbox), 8);
    gtk_widget_set_margin_bottom(GTK_WIDGET(qh->vbox), 8);
    gtk_window_set_child(qh->window, GTK_WIDGET(qh->vbox));

    /* Input row: entry + spinner */
    GtkBox *input_row = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6));
    gtk_box_append(qh->vbox, GTK_WIDGET(input_row));

    qh->entry = GTK_ENTRY(gtk_entry_new());
    if (info) {
        /* Use the last segment of wm_class (e.g. "com.mitchellh.ghostty" -> "Ghostty") */
        const char *name = info->app_name;
        const char *last_dot = strrchr(name, '.');
        if (last_dot && *(last_dot + 1))
            name = last_dot + 1;
        char *display_name = g_strdup(name);
        if (display_name[0])
            display_name[0] = g_ascii_toupper(display_name[0]);

        char *placeholder = g_strdup_printf("Ask about %s...", display_name);
        gtk_entry_set_placeholder_text(qh->entry, placeholder);
        g_free(placeholder);
        g_free(display_name);
    } else {
        gtk_entry_set_placeholder_text(qh->entry, "Ask anything...");
    }
    gtk_widget_set_hexpand(GTK_WIDGET(qh->entry), TRUE);
    g_signal_connect(qh->entry, "activate", G_CALLBACK(on_entry_activate), qh);
    gtk_box_append(input_row, GTK_WIDGET(qh->entry));

    qh->spinner = GTK_SPINNER(gtk_spinner_new());
    gtk_widget_set_visible(GTK_WIDGET(qh->spinner), FALSE);
    gtk_box_append(input_row, GTK_WIDGET(qh->spinner));

    /* Error label (hidden until an error occurs) */
    qh->error_label = GTK_LABEL(gtk_label_new(NULL));
    gtk_label_set_wrap(qh->error_label, TRUE);
    gtk_label_set_xalign(qh->error_label, 0.0);
    gtk_widget_set_visible(GTK_WIDGET(qh->error_label), FALSE);
    gtk_box_append(qh->vbox, GTK_WIDGET(qh->error_label));

    /* Scrolled response area (hidden initially) */
    qh->scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_scrolled_window_set_policy(qh->scroll, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(GTK_WIDGET(qh->scroll), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(qh->scroll), FALSE);
    gtk_box_append(qh->vbox, GTK_WIDGET(qh->scroll));

    qh->response_label = GTK_LABEL(gtk_label_new(NULL));
    gtk_label_set_wrap(qh->response_label, TRUE);
    gtk_label_set_xalign(qh->response_label, 0.0);
    gtk_label_set_yalign(qh->response_label, 0.0);
    gtk_label_set_selectable(qh->response_label, TRUE);
    gtk_scrolled_window_set_child(qh->scroll, GTK_WIDGET(qh->response_label));

    g_signal_connect(qh->window, "destroy", G_CALLBACK(on_destroy), qh);

    gtk_window_present(qh->window);
    gtk_widget_grab_focus(GTK_WIDGET(qh->entry));

    return qh;
}
