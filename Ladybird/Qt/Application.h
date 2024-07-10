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
#include <LibProtocol/RequestClient.h>
#include <LibURL/URL.h>
#include <QApplication>

namespace Ladybird {

class Application : public QApplication {
    Q_OBJECT

public:
    Application(int& argc, char** argv);
    virtual ~Application() override;

    virtual bool event(QEvent* event) override;

    Function<void(URL::URL)> on_open_file;
    RefPtr<Protocol::RequestClient> request_server_client;

    NonnullRefPtr<ImageDecoderClient::Client> image_decoder_client() const { return *m_image_decoder_client; }
    ErrorOr<void> initialize_image_decoder();

    BrowserWindow& new_window(Vector<URL::URL> const& initial_urls, WebView::CookieJar&, WebContentOptions const&, StringView webdriver_content_ipc_path, bool allow_popups, Tab* parent_tab = nullptr, Optional<u64> page_index = {});

    void show_task_manager_window(WebContentOptions const&);
    void close_task_manager_window();

    BrowserWindow& active_window() { return *m_active_window; }
    void set_active_window(BrowserWindow& w) { m_active_window = &w; }

private:
    TaskManagerWindow* m_task_manager_window { nullptr };
    BrowserWindow* m_active_window { nullptr };

    RefPtr<ImageDecoderClient::Client> m_image_decoder_client;
};

}
