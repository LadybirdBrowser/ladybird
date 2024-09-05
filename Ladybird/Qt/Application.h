/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashTable.h>
#include <Ladybird/Qt/BrowserWindow.h>
#include <LibImageDecoderClient/Client.h>
#include <LibRequests/RequestClient.h>
#include <LibURL/URL.h>
#include <LibWebView/Application.h>
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
    RefPtr<Requests::RequestClient> request_server_client;

    NonnullRefPtr<ImageDecoderClient::Client> image_decoder_client() const { return *m_image_decoder_client; }
    ErrorOr<void> initialize_image_decoder();

    BrowserWindow& new_window(Vector<URL::URL> const& initial_urls, BrowserWindow::IsPopupWindow is_popup_window = BrowserWindow::IsPopupWindow::No, Tab* parent_tab = nullptr, Optional<u64> page_index = {});

    void show_task_manager_window();
    void close_task_manager_window();

    BrowserWindow& active_window() { return *m_active_window; }
    void set_active_window(BrowserWindow& w) { m_active_window = &w; }

private:
    virtual void create_platform_arguments(Core::ArgsParser&) override;
    virtual void create_platform_options(WebView::ChromeOptions&, WebView::WebContentOptions&) override;

    virtual Optional<ByteString> ask_user_for_download_folder() const override;

    bool m_enable_qt_networking { false };

    TaskManagerWindow* m_task_manager_window { nullptr };
    BrowserWindow* m_active_window { nullptr };

    RefPtr<ImageDecoderClient::Client> m_image_decoder_client;
};

}
