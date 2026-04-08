/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <LibWebView/URL.h>
#include <UI/Gtk/Application.h>
#include <UI/Gtk/BrowserWindow.h>
#include <UI/Gtk/Dialogs.h>
#include <UI/Gtk/EventLoopImplementationGtk.h>
#include <UI/Gtk/Tab.h>
#include <UI/Gtk/WebContentView.h>

namespace Ladybird {

Application::Application() = default;
Application::~Application()
{
    m_windows.clear();
    g_clear_object(&m_adw_application);
}

NonnullOwnPtr<Core::EventLoop> Application::create_platform_event_loop()
{
    if (!browser_options().headless_mode.has_value()) {
        Core::EventLoopManager::install(*new EventLoopManagerGtk);
        adw_init();
        m_adw_application = ADW_APPLICATION(adw_application_new("org.ladybird.Ladybird",
            static_cast<GApplicationFlags>(G_APPLICATION_DEFAULT_FLAGS | G_APPLICATION_HANDLES_OPEN)));
        GError* error = nullptr;
        g_application_register(G_APPLICATION(m_adw_application), nullptr, &error);
        if (error) {
            warnln("Failed to register GApplication: {}", error->message);
            g_error_free(error);
        }

        if (g_application_get_is_remote(G_APPLICATION(m_adw_application)))
            forward_urls_to_remote_and_exit();

        setup_dbus_handlers();
    }

    auto event_loop = WebView::Application::create_platform_event_loop();

    if (!browser_options().headless_mode.has_value())
        static_cast<EventLoopImplementationGtk&>(event_loop->impl()).set_main_loop();

    return event_loop;
}

void Application::forward_urls_to_remote_and_exit()
{
    auto const& raw_urls = browser_options().raw_urls;
    if (!raw_urls.is_empty()) {
        Vector<GObjectPtr<GFile>> files;
        for (auto const& url : raw_urls)
            files.append(GObjectPtr<GFile> { g_file_new_for_commandline_arg(url.characters()) });
        Vector<GFile*> raw_files;
        for (auto& file : files)
            raw_files.append(file.ptr());
        g_application_open(G_APPLICATION(m_adw_application), raw_files.data(), static_cast<int>(raw_files.size()), "");
    } else {
        g_application_activate(G_APPLICATION(m_adw_application));
    }
    exit(0);
}

void Application::setup_dbus_handlers()
{
    g_signal_connect(m_adw_application, "open", G_CALLBACK(+[](GApplication*, GFile** files, int n_files, char const*, gpointer) {
        auto& app = Application::the();
        Vector<URL::URL> urls;
        for (int i = 0; i < n_files; i++) {
            g_autofree char* uri = g_file_get_uri(files[i]);
            if (uri) {
                if (auto url = URL::Parser::basic_parse(StringView { uri, strlen(uri) }); url.has_value())
                    urls.append(url.release_value());
            }
        }
        app.on_open(move(urls));
    }),
        nullptr);

    g_signal_connect(m_adw_application, "activate", G_CALLBACK(+[](GApplication*, gpointer) {
        Application::the().on_activate();
    }),
        nullptr);
}

void Application::on_open(Vector<URL::URL> urls)
{
    if (auto* window = active_window()) {
        for (size_t i = 0; i < urls.size(); i++)
            window->create_new_tab(urls[i], (i == 0) ? Web::HTML::ActivateTab::Yes : Web::HTML::ActivateTab::No);
        window->present();
    } else {
        new_window(urls);
    }
}

void Application::on_activate()
{
    if (auto* window = active_window())
        window->present();
    else
        new_window({});
}

BrowserWindow& Application::new_window(Vector<URL::URL> const& initial_urls)
{
    auto window = make<BrowserWindow>(m_adw_application, initial_urls);
    auto& window_ref = *window;
    m_active_window = &window_ref;

    // Track active window via focus
    g_signal_connect(window_ref.gtk_window(), "notify::is-active", G_CALLBACK(+[](GObject* gtk_window, GParamSpec*, gpointer) {
        if (!gtk_window_is_active(GTK_WINDOW(gtk_window)))
            return;
        auto& app = Application::the();
        app.for_each_window([&](BrowserWindow& bw) {
            if (GTK_WINDOW(bw.gtk_window()) == GTK_WINDOW(gtk_window))
                app.set_active_window(&bw);
        });
    }),
        nullptr);

    // Clean up when window is destroyed — defer removal to avoid mutating m_windows during iteration
    g_signal_connect(window_ref.gtk_window(), "destroy", G_CALLBACK(+[](GtkWidget* gtk_window, gpointer) {
        auto& app = Application::the();
        BrowserWindow* to_remove = nullptr;
        app.for_each_window([&](BrowserWindow& bw) {
            if (GTK_WIDGET(bw.gtk_window()) == gtk_window)
                to_remove = &bw;
        });
        if (to_remove) {
            if (app.active_window() == to_remove)
                app.set_active_window(nullptr);
            app.remove_window(*to_remove);
            bool has_windows = false;
            app.for_each_window([&](auto&) { has_windows = true; });
            if (!has_windows)
                Core::EventLoop::current().quit(0);
        }
    }),
        nullptr);

    window_ref.present();
    m_windows.append(move(window));
    return window_ref;
}

void Application::remove_window(BrowserWindow& window)
{
    m_windows.remove_first_matching([&](auto& w) { return w.ptr() == &window; });
    if (m_active_window == &window)
        m_active_window = m_windows.is_empty() ? nullptr : m_windows.last().ptr();
}

Tab* Application::active_tab() const
{
    if (!m_active_window)
        return nullptr;
    return m_active_window->current_tab();
}

Optional<WebView::ViewImplementation&> Application::active_web_view() const
{
    if (auto* tab = active_tab())
        return static_cast<WebView::ViewImplementation&>(tab->view());
    return {};
}

Optional<WebView::ViewImplementation&> Application::open_blank_new_tab(Web::HTML::ActivateTab activate_tab) const
{
    if (!m_active_window)
        return {};
    auto& tab = m_active_window->create_new_tab(activate_tab);
    return static_cast<WebView::ViewImplementation&>(tab.view());
}

Optional<ByteString> Application::ask_user_for_download_path(StringView file) const
{
    if (!m_active_window)
        return {};

    GObjectPtr dialog { gtk_file_dialog_new() };
    gtk_file_dialog_set_title(GTK_FILE_DIALOG(dialog.ptr()), "Save As");

    auto const* downloads_dir = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    if (downloads_dir) {
        GObjectPtr initial_folder { g_file_new_for_path(downloads_dir) };
        gtk_file_dialog_set_initial_folder(GTK_FILE_DIALOG(dialog.ptr()), G_FILE(initial_folder.ptr()));
    }
    gtk_file_dialog_set_initial_name(GTK_FILE_DIALOG(dialog.ptr()), ByteString(file).characters());

    Optional<ByteString> result;
    Core::EventLoop nested_loop;

    gtk_file_dialog_save(GTK_FILE_DIALOG(dialog.ptr()), m_active_window->gtk_window(), nullptr, +[](GObject* source, GAsyncResult* async_result, gpointer user_data) {
        auto* result_ptr = static_cast<Optional<ByteString>*>(user_data);
        GError* error = nullptr;
        GObjectPtr file { gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), async_result, &error) };
        if (file.ptr()) {
            g_autofree char* path = g_file_get_path(G_FILE(file.ptr()));
            if (path)
                *result_ptr = ByteString(path);
        }
        if (error)
            g_error_free(error);
        Core::EventLoop::current().quit(0); }, &result);

    nested_loop.exec();
    return result;
}

void Application::display_download_confirmation_dialog(StringView download_name, LexicalPath const& path) const
{
    if (!m_active_window)
        return;
    auto message = ByteString::formatted("{} saved to {}", download_name, path.dirname());
    auto* toast = adw_toast_new(message.characters());
    adw_toast_set_timeout(toast, 5);
    m_active_window->show_toast(toast);
}

void Application::display_error_dialog(StringView error_message) const
{
    if (!m_active_window)
        return;
    Dialogs::show_error(m_active_window->gtk_window(), error_message);
}

// GDK4 only provides an async clipboard API. Spin a nested event loop to read synchronously.
static Optional<ByteString> read_clipboard_text_sync()
{
    auto* clipboard = gdk_display_get_clipboard(gdk_display_get_default());

    Optional<ByteString> result;
    Core::EventLoop nested_loop;

    gdk_clipboard_read_text_async(clipboard, nullptr, [](GObject* source, GAsyncResult* async_result, gpointer user_data) {
        auto* result_ptr = static_cast<Optional<ByteString>*>(user_data);
        g_autofree char* text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), async_result, nullptr);
        if (text)
            *result_ptr = ByteString(text);
        Core::EventLoop::current().quit(0); }, &result);

    nested_loop.exec();
    return result;
}

Utf16String Application::clipboard_text() const
{
    if (browser_options().headless_mode.has_value())
        return WebView::Application::clipboard_text();

    if (auto text = read_clipboard_text_sync(); text.has_value())
        return Utf16String::from_utf8(text->view());
    return {};
}

Vector<Web::Clipboard::SystemClipboardRepresentation> Application::clipboard_entries() const
{
    if (browser_options().headless_mode.has_value())
        return WebView::Application::clipboard_entries();

    Vector<Web::Clipboard::SystemClipboardRepresentation> representations;
    if (auto text = read_clipboard_text_sync(); text.has_value())
        representations.empend(text.release_value(), MUST(String::from_utf8("text/plain"sv)));
    return representations;
}

void Application::insert_clipboard_entry(Web::Clipboard::SystemClipboardRepresentation entry)
{
    if (browser_options().headless_mode.has_value()) {
        WebView::Application::insert_clipboard_entry(move(entry));
        return;
    }
    auto* clipboard = gdk_display_get_clipboard(gdk_display_get_default());
    if (entry.mime_type == "text/plain"sv)
        gdk_clipboard_set_text(clipboard, entry.data.characters());
}

void Application::rebuild_bookmarks_menu() const
{
}

void Application::update_bookmarks_bar_display(bool) const
{
}

void Application::on_devtools_enabled() const
{
    WebView::Application::on_devtools_enabled();
    for (auto& window : m_windows)
        window->on_devtools_enabled();
}

void Application::on_devtools_disabled() const
{
    WebView::Application::on_devtools_disabled();
    for (auto& window : m_windows)
        window->on_devtools_disabled();
}

}
