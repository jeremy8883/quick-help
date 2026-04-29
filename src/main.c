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

static gboolean opt_no_decorations = FALSE;
static gboolean opt_screenshot = FALSE;
static char *opt_model = NULL;

static GOptionEntry option_entries[] = {
    { "no-decorations", 0, 0, G_OPTION_ARG_NONE, &opt_no_decorations,
      "Hide window decorations (title bar)", NULL },
    { "screenshot", 's', 0, G_OPTION_ARG_NONE, &opt_screenshot,
      "Capture and attach a screenshot of the focused window", NULL },
    { "model", 'm', 0, G_OPTION_ARG_STRING, &opt_model,
      "Default model ID (e.g. claude-sonnet-4-6)", "MODEL" },
    { NULL }
};

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

    /* Capture screenshot of focused window before our window appears */
    char *screenshot_path = NULL;
    if (opt_screenshot) {
        char *fname = g_strdup_printf("qh-screenshot-%d.png", getpid());
        screenshot_path = g_build_filename(g_get_tmp_dir(), fname, NULL);
        g_free(fname);
        gchar *argv[] = { "gnome-screenshot", "-w", "-f", screenshot_path, NULL };
        GError *ss_error = NULL;
        gint exit_status;
        if (!g_spawn_sync(NULL, argv, NULL,
                          G_SPAWN_SEARCH_PATH,
                          NULL, NULL, NULL, NULL,
                          &exit_status, &ss_error) ||
            !g_spawn_check_wait_status(exit_status, &ss_error)) {
            g_warning("gnome-screenshot failed: %s", ss_error->message);
            g_error_free(ss_error);
            g_free(screenshot_path);
            screenshot_path = NULL;
        } else {
            g_message("Screenshot saved: %s", screenshot_path);
        }
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

    /* Optional: Brave Search API key for web search tool */
    const char *brave_key = g_getenv("BRAVE_SEARCH_API_KEY");
    if (brave_key && *brave_key)
        backend->brave_api_key = g_strdup(brave_key);

    /* Create and show the window */
    quick_help_window_new(app, backend, info, sys, opt_no_decorations, opt_model,
                          screenshot_path);
    g_free(screenshot_path);
}

int main(int argc, char *argv[]) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    GtkApplication *app = gtk_application_new("com.github.quick-help",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_application_add_main_option_entries(G_APPLICATION(app), option_entries);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);
    curl_global_cleanup();
    return status;
}
