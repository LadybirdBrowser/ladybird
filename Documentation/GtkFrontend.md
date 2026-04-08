# GTK Frontend

GTK4/libadwaita frontend for Ladybird.

## Style Guide

Follow the project [Coding Style](CodingStyle.md). Prefer C++ over C idioms wherever possible. Below are GTK-specific rules.

### General
- Do not duplicate functionality already in LibWebView — the frontend is a thin shell
- Follow [GNOME HIG](https://developer.gnome.org/hig/) — prefer libadwaita widgets (`AdwTabView`, `AdwHeaderBar`, `AdwAlertDialog`, `AdwToast`, etc.) over custom implementations
- Prefer C++ types and patterns over GLib/GObject equivalents
- Use D-Bus via `GApplication` for single-instance and IPC — not custom socket/file mechanisms
- Prefer file-static functions over anonymous namespaces for internal helpers
- Namespace: `Ladybird::` for C++ classes, `LadybirdWidgets::` for GObject widget helpers

### GObject
- Only register GTypes (`G_DEFINE_FINAL_TYPE`) for widgets used in GtkBuilder templates
- No `G_DECLARE_FINAL_TYPE` — define structs and `GType` functions manually to avoid reserved `_TypeName` names
- Use `GtkBuilder` and `.ui` resource files for declarative layout where practical

### Signals and Callbacks
- No `bind_template_callback` — connect signals in C++ with `g_signal_connect_swapped`
- No `g_signal_new` — use `Function<>` callbacks instead of custom GObject signals
- Prefer `g_signal_connect_swapped` over `g_signal_connect` to avoid `static_cast<Foo*>(user_data)`
- For buttons in popover menus, prefer `action-name` properties over click signal handlers

### Memory Management
- No manual `g_object_unref` — use `GObjectPtr<T>` (see `UI/Gtk/GLibPtr.h`)
- No manual `g_free` — use `g_autofree`

```cpp
// Local scope — auto-unrefs when out of scope
GObjectPtr builder { gtk_builder_new_from_resource("/path/to/file.ui") };

// Class member
GObjectPtr<GdkTexture> m_texture;
m_texture = GObjectPtr<GdkTexture> { gdk_memory_texture_builder_build(builder) };

// g_autofree for glib-allocated strings
g_autofree char* path = g_file_get_path(file);
```

## Test Checklist

### Navigation
- [ ] Load URL from location bar
- [ ] Back/forward buttons
- [ ] Reload (Ctrl+R, F5)
- [ ] New tab (Ctrl+T)
- [ ] Close tab (Ctrl+W)
- [ ] New window (Ctrl+N)
- [ ] Tab switching
- [ ] Child tab creation (target=_blank)

### Input
- [ ] Keyboard input in web content
- [ ] Mouse clicks, drag, selection
- [ ] Touchpad smooth scrolling
- [ ] Mouse wheel discrete scrolling
- [ ] Ctrl+scroll zoom
- [ ] Right-click context menus (page, link, image, media)

### UI
- [ ] Location bar URL display with domain highlighting
- [ ] Location bar autocomplete
- [ ] Security icon (https/insecure)
- [ ] Find in page (Ctrl+F)
- [ ] Fullscreen (F11)
- [ ] Zoom in/out/reset (Ctrl+=/-/0)
- [ ] Tab loading indicator
- [ ] Cursor changes (pointer, text, resize, etc.)
- [ ] Tooltips on hover
- [ ] Dark/light theme follows system

### Dialogs
- [ ] JavaScript alert
- [ ] JavaScript confirm
- [ ] JavaScript prompt
- [ ] Color picker
- [ ] File picker (single and multiple)
- [ ] Select dropdown
- [ ] Download save dialog
- [ ] Download confirmation toast
- [ ] Error dialog

### Window Management
- [ ] Minimize/maximize/close
- [ ] Resize
- [ ] Fullscreen enter/exit
- [ ] D-Bus single instance (second launch opens tab in existing window)

### Internal Pages
- [ ] about:version
- [ ] about:settings
- [ ] about:processes (Task Manager)

### Clipboard
- [ ] Copy (Ctrl+C)
- [ ] Paste (Ctrl+V)
- [ ] Select all (Ctrl+A)

### DevTools
- [ ] Enable/disable via banner
- [ ] Toggle (Ctrl+Shift+I, F12)
