/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibURL/URL.h>
#include <LibWebView/Application.h>
#include <UI/Qt/BrowserWindow.h>

#include <QApplication>

namespace Ladybird {

class Application : public WebView::Application {
    WEB_VIEW_APPLICATION(Application)

public:
    virtual ~Application() override;

    Function<void(URL::URL)> on_open_file;

    BrowserWindow& new_window(Vector<URL::URL> const& initial_urls, BrowserWindow::IsPopupWindow is_popup_window = BrowserWindow::IsPopupWindow::No, Tab* parent_tab = nullptr, Optional<u64> page_index = {});

    BrowserWindow& active_window() const { return *m_active_window; }
    void set_active_window(BrowserWindow& w) { m_active_window = &w; }

    Tab* active_tab() const { return m_active_window ? m_active_window->current_tab() : nullptr; }

private:
    explicit Application();

    virtual void create_platform_options(WebView::BrowserOptions&, WebView::WebContentOptions&) override;
    virtual NonnullOwnPtr<Core::EventLoop> create_platform_event_loop() override;

    virtual Optional<WebView::ViewImplementation&> active_web_view() const override;
    virtual Optional<WebView::ViewImplementation&> open_blank_new_tab(Web::HTML::ActivateTab) const override;

    virtual Optional<ByteString> ask_user_for_download_folder() const override;
    virtual void display_download_confirmation_dialog(StringView download_name, LexicalPath const& path) const override;
    virtual void display_error_dialog(StringView error_message) const override;

    virtual void on_devtools_enabled() const override;
    virtual void on_devtools_disabled() const override;

    OwnPtr<QApplication> m_application;
    BrowserWindow* m_active_window { nullptr };
};

}
