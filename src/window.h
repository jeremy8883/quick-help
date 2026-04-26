#ifndef WINDOW_H
#define WINDOW_H

#include <gtk/gtk.h>
#include "ai.h"
#include "config.h"
#include "window_context.h"
#include "system_context.h"

typedef struct _QuickHelpWindow QuickHelpWindow;

/* Create the quick-help popup window.
 * Takes ownership of backend, info, and sys (will free them on destroy). */
QuickHelpWindow *quick_help_window_new(GtkApplication *app,
                                        AiBackend *backend,
                                        WindowInfo *info,
                                        SystemContext *sys,
                                        QuickHelpConfig *config);

#endif
