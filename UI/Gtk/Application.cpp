/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Gtk/Application.h>
#include <UI/Gtk/EventLoopImplementationGtk.h>

namespace Ladybird {

Application::Application() = default;
Application::~Application()
{
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
    }

    auto event_loop = WebView::Application::create_platform_event_loop();

    if (!browser_options().headless_mode.has_value())
        static_cast<EventLoopImplementationGtk&>(event_loop->impl()).set_main_loop();

    return event_loop;
}

Optional<WebView::ViewImplementation&> Application::active_web_view() const { return {}; }
Optional<WebView::ViewImplementation&> Application::open_blank_new_tab(Web::HTML::ActivateTab) const { return {}; }
Optional<ByteString> Application::ask_user_for_download_path(StringView) const { return {}; }
void Application::display_download_confirmation_dialog(StringView, LexicalPath const&) const { }
void Application::display_error_dialog(StringView) const { }
Utf16String Application::clipboard_text() const { return {}; }
Vector<Web::Clipboard::SystemClipboardRepresentation> Application::clipboard_entries() const { return {}; }
void Application::insert_clipboard_entry(Web::Clipboard::SystemClipboardRepresentation) { }
void Application::rebuild_bookmarks_menu() const { }
void Application::update_bookmarks_bar_display(bool) const { }
void Application::on_devtools_enabled() const { }
void Application::on_devtools_disabled() const { }

}
