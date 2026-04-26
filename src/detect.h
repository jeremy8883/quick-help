#ifndef DETECT_H
#define DETECT_H

typedef struct {
    char *app_name;   /* wm_class e.g. "Blender" */
    char *title;      /* window title e.g. "untitled.blend" */
} WindowInfo;

/* Detect the currently focused window via window-calls D-Bus extension.
 * Returns a heap-allocated WindowInfo (caller must free with window_info_free),
 * or NULL on failure. */
WindowInfo *detect_focused_window(void);

void window_info_free(WindowInfo *info);

#endif
