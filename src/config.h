#ifndef CONFIG_H
#define CONFIG_H

#include <glib.h>

typedef struct {
    gboolean hide_decorations;
} QuickHelpConfig;

/* Load config from ~/.config/quick-help/config (GKeyFile format).
 * Returns defaults if file doesn't exist. Caller must free with config_free(). */
QuickHelpConfig *config_load(void);
void config_free(QuickHelpConfig *config);

#endif
