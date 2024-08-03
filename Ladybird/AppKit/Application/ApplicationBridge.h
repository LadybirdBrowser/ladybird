/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibIPC/Forward.h>
#include <LibWebView/Application.h>
#include <LibWebView/Forward.h>

namespace Ladybird {

struct ApplicationBridgeImpl;
class WebViewBridge;

class ApplicationBridge : public WebView::Application {
    WEB_VIEW_APPLICATION(ApplicationBridge)

public:
    ~ApplicationBridge();

    ErrorOr<void> launch_request_server();
    ErrorOr<void> launch_image_decoder();
    ErrorOr<NonnullRefPtr<WebView::WebContentClient>> launch_web_content(WebViewBridge&);
    ErrorOr<IPC::File> launch_web_worker();

    void dump_connection_info();

private:
    NonnullOwnPtr<ApplicationBridgeImpl> m_impl;
};

}
