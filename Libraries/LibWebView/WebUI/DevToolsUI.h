/*
 * Copyright (c) 2025, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibDevTools/CallbackTransport.h>
#include <LibWebView/Forward.h>
#include <LibWebView/WebUI.h>

namespace WebView {

class WEBVIEW_API DevToolsUI : public WebUI {
    WEB_UI(DevToolsUI);

public:
    static ErrorOr<NonnullRefPtr<DevToolsUI>> create_with_inspected_view_id(WebContentClient& client, String host, Optional<u64> inspected_view_id);
    virtual ~DevToolsUI() override;

private:
    DevToolsUI(WebContentClient& client, NonnullOwnPtr<IPC::Transport> transport, String host, Optional<u64> inspected_view_id);
    virtual void register_interfaces() override;
    void get_inspected_browser_id();
    void load_remote_debugging_settings();
    void set_remote_debugging_settings(JsonValue const&);

    Optional<u64> m_inspected_view_id;
    RefPtr<DevTools::CallbackTransport> m_transport;
};

}
