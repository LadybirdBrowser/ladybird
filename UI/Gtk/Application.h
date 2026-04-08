/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWebView/Application.h>

#include <adwaita.h>

namespace Ladybird {

class BrowserWindow;
class Tab;

class Application : public WebView::Application {
    WEB_VIEW_APPLICATION(Application)

public:
    virtual ~Application() override;

    BrowserWindow& new_window(Vector<URL::URL> const& initial_urls);
    void remove_window(BrowserWindow&);

    BrowserWindow* active_window() const { return m_active_window; }
    void set_active_window(BrowserWindow* w) { m_active_window = w; }

    Tab* active_tab() const;

    AdwApplication* adw_application() const { return m_adw_application; }

    template<typename Callback>
    void for_each_window(Callback callback)
    {
        for (auto& window : m_windows)
            callback(*window);
    }

private:
    explicit Application();

    virtual NonnullOwnPtr<Core::EventLoop> create_platform_event_loop() override;

    virtual Optional<WebView::ViewImplementation&> active_web_view() const override;
    virtual Optional<WebView::ViewImplementation&> open_blank_new_tab(Web::HTML::ActivateTab) const override;

    virtual Optional<ByteString> ask_user_for_download_path(StringView file) const override;
    virtual void display_download_confirmation_dialog(StringView download_name, LexicalPath const& path) const override;
    virtual void display_error_dialog(StringView error_message) const override;

    virtual Utf16String clipboard_text() const override;
    virtual Vector<Web::Clipboard::SystemClipboardRepresentation> clipboard_entries() const override;
    virtual void insert_clipboard_entry(Web::Clipboard::SystemClipboardRepresentation) override;

    virtual bool should_capture_web_content_output() const override { return false; }

    virtual void rebuild_bookmarks_menu() const override;
    virtual void update_bookmarks_bar_display(bool) const override;

    virtual void on_devtools_enabled() const override;
    virtual void on_devtools_disabled() const override;

    void forward_urls_to_remote_and_exit();
    void setup_dbus_handlers();
    void on_open(Vector<URL::URL> urls);
    void on_activate();

    AdwApplication* m_adw_application { nullptr };
    Vector<NonnullOwnPtr<BrowserWindow>> m_windows;
    BrowserWindow* m_active_window { nullptr };
};

}
