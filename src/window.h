#ifndef WINDOW_H
#define WINDOW_H

#include <gtk/gtk.h>
#include "ai.h"
#include "detect.h"

typedef struct _QuickHelpWindow QuickHelpWindow;

/* Create the quick-help popup window.
 * Takes ownership of backend and info (will free them on destroy). */
QuickHelpWindow *quick_help_window_new(GtkApplication *app,
                                        AiBackend *backend,
                                        WindowInfo *info);

#endif
