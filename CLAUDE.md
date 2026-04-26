# Quick Help

Small native GTK4/C app. Single-window popup for AI-assisted help with automatic window context detection.

## Architecture

- `src/ai.c/h` — AI backend interface (function pointers). Claude implementation exists; interface is backend-agnostic for future providers.
- `src/ai_web_tool.c/h` — URL fetching tool (always available)
- `src/ai_search_tool.c/h` — Brave Search tool (optional, requires BRAVE_SEARCH_API_KEY)
- `src/window.c/h` — GTK4 UI: state, streaming, rendering, construction
- `src/window_internal.h` — Shared struct and declarations between window.c and input.c
- `src/input.c` — Keyboard handler dispatch and individual key handlers
- `src/window_context.c/h` — D-Bus call to `org.gnome.Shell.Extensions.Windows.List()` (window-calls GNOME extension)
- `src/markdown.c/h` — Markdown to Pango markup conversion
- `src/main.c` — Wiring: window context -> create backend -> show UI

## Build

`meson setup build && ninja -C build`

Dependencies: gtk4, libcurl, json-glib-1.0

## Key constraints

- API call runs in a thread; UI updates via `g_idle_add` back to main thread
- Multiple tool calls from a single response execute in parallel (separate threads)
- No chat persistence — conversation lives in memory only, gone on window close
- Window detection happens once at launch (before our window takes focus)
