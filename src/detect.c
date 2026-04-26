#include "detect.h"
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>

void window_info_free(WindowInfo *info) {
    if (!info) return;
    g_free(info->app_name);
    g_free(info->title);
    g_free(info);
}

WindowInfo *detect_focused_window(void) {
    GError *error = NULL;
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!conn) {
        g_warning("Failed to connect to session bus: %s", error->message);
        g_error_free(error);
        return NULL;
    }

    GVariant *result = g_dbus_connection_call_sync(
        conn,
        "org.gnome.Shell",
        "/org/gnome/Shell/Extensions/Windows",
        "org.gnome.Shell.Extensions.Windows",
        "List",
        NULL,
        G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE,
        3000, /* 3 second timeout */
        NULL,
        &error
    );

    if (!result) {
        g_warning("D-Bus call to window-calls failed: %s", error->message);
        g_error_free(error);
        g_object_unref(conn);
        return NULL;
    }

    const gchar *json_str = NULL;
    g_variant_get(result, "(&s)", &json_str);

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json_str, -1, &error)) {
        g_warning("Failed to parse window list JSON: %s", error->message);
        g_error_free(error);
        g_object_unref(parser);
        g_variant_unref(result);
        g_object_unref(conn);
        return NULL;
    }

    JsonNode *root = json_parser_get_root(parser);
    JsonArray *windows = json_node_get_array(root);
    WindowInfo *info = NULL;

    for (guint i = 0; i < json_array_get_length(windows); i++) {
        JsonObject *win = json_array_get_object_element(windows, i);
        if (json_object_has_member(win, "focus") &&
            json_object_get_boolean_member(win, "focus")) {
            info = g_new0(WindowInfo, 1);
            if (json_object_has_member(win, "wm_class"))
                info->app_name = g_strdup(json_object_get_string_member(win, "wm_class"));
            else
                info->app_name = g_strdup("Unknown");
            if (json_object_has_member(win, "title"))
                info->title = g_strdup(json_object_get_string_member(win, "title"));
            else
                info->title = g_strdup("");
            break;
        }
    }

    g_object_unref(parser);
    g_variant_unref(result);
    g_object_unref(conn);
    return info;
}
