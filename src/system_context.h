#ifndef SYSTEM_CONTEXT_H
#define SYSTEM_CONTEXT_H

typedef struct {
    char *os_name;        /* e.g. "Fedora Linux 43" or "Ubuntu 24.04" */
    char *desktop_env;    /* e.g. "GNOME", "KDE" */
    char *display_server; /* "wayland" or "x11" */
    char *shell;          /* e.g. "zsh", "bash" */
} SystemContext;

/* Gather system context from env vars and /etc/os-release.
 * Returns a heap-allocated SystemContext (caller must free with system_context_free). */
SystemContext *detect_system_context(void);

void system_context_free(SystemContext *ctx);

#endif
