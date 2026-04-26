#include <gtk/gtk.h>
#include <curl/curl.h>
#include "ai.h"
#include "window_context.h"
#include "system_context.h"
#include "window.h"

static void on_alert_dismissed(GObject *source, GAsyncResult *res, gpointer user_data) {
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source);
    GApplication *app = user_data;
    gtk_alert_dialog_choose_finish(alert, res, NULL);
    g_application_release(app);
}

static void on_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;

    /* Check for API key */
    const char *api_key = g_getenv("ANTHROPIC_API_KEY");
    if (!api_key || !*api_key) {
        GtkAlertDialog *alert = gtk_alert_dialog_new(
            "ANTHROPIC_API_KEY environment variable is not set.\n\n"
            "Set it with:\n  export ANTHROPIC_API_KEY=sk-ant-...");
        g_application_hold(G_APPLICATION(app));
        gtk_alert_dialog_choose(alert, NULL, NULL, on_alert_dismissed, app);
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

    /* Gather system context */
    SystemContext *sys = detect_system_context();
    g_message("System: %s, DE: %s, Display: %s, Shell: %s",
              sys->os_name,
              sys->desktop_env ? sys->desktop_env : "(unknown)",
              sys->display_server ? sys->display_server : "(unknown)",
              sys->shell ? sys->shell : "(unknown)");

    /* Create AI backend */
    AiBackend *backend = ai_claude_new(api_key);

    /* Create and show the window */
    quick_help_window_new(app, backend, info, sys);
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
