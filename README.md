# Quick Help

A tiny native Linux app that gives you instant AI help about whatever application you're currently using. Press a keyboard shortcut, type your question, get a concise answer.

It's essentially an LLM wrapper which provides:
- A quick, minimal interface, with everything keyboard accessible
- Information about your OS and currently focused window is provided to the LLM, so you never need to ask "in Blender, how do I move items" - just ask "move objects"
- Can fetch and read web pages when it needs up-to-date information

This application has been specifically built for my own environment, Fedora with GNOME. If you'd like your own distro supported, create a GH issue.

*Disclaimer: this codebase is vibe-coded, but I use the application personally, and am on-top of any bugs and UI concerns.*

## Build and Install

* Install the [window-calls](https://github.com/ickyicky/window-calls) GNOME extension, which is used to detect the currently focused window.

* Install 3rd party deps GTK4, libcurl, json-glib, and meson.

```bash
# Fedora
sudo dnf install gtk4-devel libcurl-devel json-glib-devel meson ninja-build
```

* Build it

```bash
meson setup build
ninja -C build
```

* Generate an Anthropic API key [here](https://console.anthropic.com/settings/keys), and add to your .bashrc/.zshrc:

```bash
export ANTHROPIC_API_KEY=sk-ant-...
```

* Add a custom shortcut under **Settings > Keyboard > Custom Shortcuts** and set the command to the full path of the binary. eg. `/home/you/Applications/quick-help/build/quick-help`
