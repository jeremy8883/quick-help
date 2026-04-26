#include "system_context.h"
#include <glib.h>
#include <string.h>

static char *read_os_name(void) {
    char *contents = NULL;
    if (!g_file_get_contents("/etc/os-release", &contents, NULL, NULL))
        return g_strdup("Linux");

    /* Look for PRETTY_NAME="..." */
    char *pretty = strstr(contents, "PRETTY_NAME=");
    if (!pretty) {
        g_free(contents);
        return g_strdup("Linux");
    }

    pretty += strlen("PRETTY_NAME=");
    /* Strip optional quotes */
    if (*pretty == '"') pretty++;
    char *end = strchr(pretty, '\n');
    if (!end) end = pretty + strlen(pretty);
    /* Strip trailing quote */
    if (end > pretty && *(end - 1) == '"') end--;

    char *result = g_strndup(pretty, end - pretty);
    g_free(contents);
    return result;
}

static char *get_shell_name(void) {
    const char *shell = g_getenv("SHELL");
    if (!shell) return NULL;

    const char *base = strrchr(shell, '/');
    return g_strdup(base ? base + 1 : shell);
}

SystemContext *detect_system_context(void) {
    SystemContext *ctx = g_new0(SystemContext, 1);

    ctx->os_name = read_os_name();

    const char *desktop = g_getenv("XDG_CURRENT_DESKTOP");
    ctx->desktop_env = g_strdup(desktop ? desktop : NULL);

    const char *session = g_getenv("XDG_SESSION_TYPE");
    ctx->display_server = g_strdup(session ? session : NULL);

    ctx->shell = get_shell_name();

    return ctx;
}

void system_context_free(SystemContext *ctx) {
    if (!ctx) return;
    g_free(ctx->os_name);
    g_free(ctx->desktop_env);
    g_free(ctx->display_server);
    g_free(ctx->shell);
    g_free(ctx);
}
