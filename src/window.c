#include "window.h"
#include "system_context.h"
#include "markdown.h"
#include <string.h>

#define INITIAL_WIDTH  500
#define INITIAL_HEIGHT 60
#define EXPANDED_HEIGHT 500
#define MAX_MESSAGES 50

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

typedef struct {
    QuickHelpWindow *qh;
    char *question;
} SendTask;

static gboolean on_response_ready(gpointer data) {
    SendTask *task = data;
    QuickHelpWindow *qh = task->qh;

    /* Rebuild full conversation display */
    {
        GString *display = g_string_new(NULL);
        for (int i = 0; i < qh->msg_count; i++) {
            if (g_strcmp0(qh->messages[i].role, "user") == 0) {
                char *escaped = g_markup_escape_text(qh->messages[i].content, -1);
                g_string_append_printf(display, "<b>You:</b> %s\n\n", escaped);
                g_free(escaped);
            } else {
                char *pango = markdown_to_pango(qh->messages[i].content);
                g_string_append_printf(display, "%s\n\n", pango);
                g_free(pango);
            }
        }
        if (display->len > 2)
            g_string_truncate(display, display->len - 2);
        gtk_label_set_markup(qh->response_label, display->str);
        g_string_free(display, TRUE);
    }

    gtk_spinner_stop(qh->spinner);
    gtk_widget_set_visible(GTK_WIDGET(qh->spinner), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(qh->entry), TRUE);
    gtk_widget_grab_focus(GTK_WIDGET(qh->entry));

    g_free(task->question);
    g_free(task);
    return G_SOURCE_REMOVE;
}

static gpointer send_thread(gpointer data) {
    SendTask *task = data;
    QuickHelpWindow *qh = task->qh;

    char *response = qh->backend->send(
        qh->backend, qh->system_prompt,
        qh->messages, qh->msg_count
    );

    /* Append assistant message */
    if (qh->msg_count < MAX_MESSAGES) {
        qh->messages[qh->msg_count].role = g_strdup("assistant");
        qh->messages[qh->msg_count].content = response ? response : g_strdup("(No response)");
        qh->msg_count++;
    } else {
        g_free(response);
    }

    g_idle_add(on_response_ready, task);
    return NULL;
}

static void on_submit(QuickHelpWindow *qh) {
    const char *text = gtk_editable_get_text(GTK_EDITABLE(qh->entry));
    if (!text || !*text) return;

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

    /* Update label to show conversation so far */
    GString *display = g_string_new(NULL);
    for (int i = 0; i < qh->msg_count; i++) {
        if (g_strcmp0(qh->messages[i].role, "user") == 0) {
            char *escaped = g_markup_escape_text(qh->messages[i].content, -1);
            g_string_append_printf(display, "<b>You:</b> %s\n\n", escaped);
            g_free(escaped);
        } else {
            char *pango = markdown_to_pango(qh->messages[i].content);
            g_string_append_printf(display, "%s\n\n", pango);
            g_free(pango);
        }
    }
    g_string_append(display, "<i>Thinking...</i>");
    gtk_label_set_markup(qh->response_label, display->str);
    g_string_free(display, TRUE);

    /* Send in background thread */
    SendTask *task = g_new0(SendTask, 1);
    task->qh = qh;
    task->question = g_strdup(qh->messages[qh->msg_count - 1].content);
    g_thread_unref(g_thread_new("ai-send", send_thread, task));
}

static void on_entry_activate(GtkEntry *entry, gpointer data) {
    (void)entry;
    on_submit(data);
}

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer data) {
    (void)ctrl; (void)keycode; (void)state;
    if (keyval == GDK_KEY_Escape) {
        QuickHelpWindow *qh = data;
        gtk_window_close(qh->window);
        return TRUE;
    }
    return FALSE;
}

QuickHelpWindow *quick_help_window_new(GtkApplication *app,
                                        AiBackend *backend,
                                        WindowInfo *info,
                                        SystemContext *sys) {
    QuickHelpWindow *qh = g_new0(QuickHelpWindow, 1);
    qh->backend = backend;
    qh->info = info;
    qh->sys = sys;

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
        "Give concise, actionable answers. "
        "Prioritize keyboard shortcuts first, then menu navigation. "
        "Keep responses brief - the user wants quick answers, not tutorials. "
        "Use markdown formatting: bold for emphasis, backticks for keyboard shortcuts and code.");

    qh->system_prompt = g_string_free(prompt, FALSE);

    /* Create window */
    qh->window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(qh->window, "Quick Help");
    gtk_window_set_default_size(qh->window, INITIAL_WIDTH, INITIAL_HEIGHT);
    gtk_window_set_resizable(qh->window, TRUE);

    /* Escape key handler */
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
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

    /* Scrolled response area (hidden initially) */
    qh->scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_scrolled_window_set_policy(qh->scroll, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(GTK_WIDGET(qh->scroll), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(qh->scroll), FALSE);
    gtk_box_append(qh->vbox, GTK_WIDGET(qh->scroll));

    qh->response_label = GTK_LABEL(gtk_label_new(NULL));
    gtk_label_set_wrap(qh->response_label, TRUE);
    gtk_label_set_xalign(qh->response_label, 0.0);
    gtk_label_set_selectable(qh->response_label, TRUE);
    gtk_scrolled_window_set_child(qh->scroll, GTK_WIDGET(qh->response_label));

    g_signal_connect(qh->window, "destroy", G_CALLBACK(on_destroy), qh);

    gtk_window_present(qh->window);
    gtk_widget_grab_focus(GTK_WIDGET(qh->entry));

    return qh;
}
