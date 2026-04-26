#include "config.h"

QuickHelpConfig *config_load(void) {
    QuickHelpConfig *config = g_new0(QuickHelpConfig, 1);

    char *path = g_build_filename(g_get_user_config_dir(),
                                  "quick-help", "config", NULL);
    GKeyFile *kf = g_key_file_new();

    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        config->hide_decorations =
            g_key_file_get_boolean(kf, "window", "hide_decorations", NULL);
    }

    g_key_file_free(kf);
    g_free(path);
    return config;
}

void config_free(QuickHelpConfig *config) {
    g_free(config);
}
