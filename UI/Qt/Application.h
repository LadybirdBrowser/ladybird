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

class Application
    : public QApplication
    , public WebView::Application {
    Q_OBJECT
    WEB_VIEW_APPLICATION(Application)

public:
    virtual ~Application() override;

    virtual bool event(QEvent* event) override;

    Function<void(URL::URL)> on_open_file;

    BrowserWindow& new_window(Vector<URL::URL> const& initial_urls, BrowserWindow::IsPopupWindow is_popup_window = BrowserWindow::IsPopupWindow::No, Tab* parent_tab = nullptr, Optional<u64> page_index = {});

    BrowserWindow& active_window() { return *m_active_window; }
    void set_active_window(BrowserWindow& w) { m_active_window = &w; }

private:
    virtual void create_platform_options(WebView::BrowserOptions&, WebView::WebContentOptions&) override;

    virtual Optional<ByteString> ask_user_for_download_folder() const override;

    BrowserWindow* m_active_window { nullptr };
};

}
