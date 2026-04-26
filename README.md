# Quick Help

A tiny native Linux app that gives you instant AI help about whatever application you're currently using. Press a keyboard shortcut, type your question, get a concise answer — keyboard shortcuts first, menu navigation second.

Uses the [window-calls](https://github.com/ickyicky/window-calls) GNOME extension to detect the focused window, so you never have to say "in Blender, how do I..." — just ask.

## Build

Requires GTK4, libcurl, json-glib, and meson.

```bash
# Fedora
sudo dnf install gtk4-devel libcurl-devel json-glib-devel meson ninja-build

# Ubuntu/Debian
sudo apt install libgtk-4-dev libcurl4-openssl-dev libjson-glib-dev meson ninja-build
```

```bash
meson setup build
ninja -C build
```

## Usage

```bash
export ANTHROPIC_API_KEY=sk-ant-...
./build/quick-help
```

Then bind it to a keyboard shortcut:
**Settings > Keyboard > Custom Shortcuts** — set the command to the full path of the binary.

### How it works

1. Press your shortcut — app detects the focused window via D-Bus
2. Small text field appears with context-aware placeholder
3. Type a question, press Enter — window expands with the AI response
4. Ask follow-ups or press Escape to close (conversation is not saved)

## Requirements

- GNOME with [window-calls](https://github.com/ickyicky/window-calls) extension enabled
- `ANTHROPIC_API_KEY` environment variable set — generate one at [console.anthropic.com/settings/keys](https://console.anthropic.com/settings/keys)
