#include <gtk/gtk.h>
#include <curl/curl.h>
#include "ai.h"
#include "window_context.h"
#include "window.h"

static void on_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;

    /* Check for API key */
    const char *api_key = g_getenv("ANTHROPIC_API_KEY");
    if (!api_key || !*api_key) {
        GtkAlertDialog *alert = gtk_alert_dialog_new(
            "ANTHROPIC_API_KEY environment variable is not set.\n\n"
            "Set it with:\n  export ANTHROPIC_API_KEY=sk-ant-...");
        gtk_alert_dialog_show(alert, NULL);
        g_object_unref(alert);
        return;
    }

    /* Detect currently focused window (before our window appears) */
    WindowInfo *info = detect_focused_window();
    if (info) {
        g_message("Detected: %s - %s", info->app_name, info->title);
    } else {
        g_message("Could not detect focused window (window-calls extension may not be running)");
    }

    /* Create AI backend */
    AiBackend *backend = ai_claude_new(api_key);

    /* Create and show the window */
    quick_help_window_new(app, backend, info);
}

int main(int argc, char *argv[]) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    GtkApplication *app = gtk_application_new("com.github.quick-help",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);
    curl_global_cleanup();
    return status;
}
