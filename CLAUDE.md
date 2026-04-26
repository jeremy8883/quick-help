# Quick Help

Small native GTK4/C app. Single-window popup for AI-assisted help with automatic window context detection.

## Architecture

- `src/ai.c/h` — AI backend interface (function pointers). Claude implementation exists; interface is backend-agnostic for future providers.
- `src/window_context.c/h` — D-Bus call to `org.gnome.Shell.Extensions.Windows.List()` (window-calls GNOME extension)
- `src/window.c/h` — GTK4 UI. Starts small (entry only), expands on first response.
- `src/markdown.c/h` — Markdown to Pango markup conversion
- `src/main.c` — Wiring: window context -> create backend -> show UI

## Build

`meson setup build && ninja -C build`

Dependencies: gtk4, libcurl, json-glib-1.0

## Key constraints

- API call runs in a thread; UI updates via `g_idle_add` back to main thread
- No chat persistence — conversation lives in memory only, gone on window close
- Window detection happens once at launch (before our window takes focus)
