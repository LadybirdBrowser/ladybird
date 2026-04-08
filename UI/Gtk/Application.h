/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/Application.h>

#include <adwaita.h>

namespace Ladybird {

class Application : public WebView::Application {
    WEB_VIEW_APPLICATION(Application)

public:
    virtual ~Application() override;

    AdwApplication* adw_application() const { return m_adw_application; }

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

    AdwApplication* m_adw_application { nullptr };
};

}
