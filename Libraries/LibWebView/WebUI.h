/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/JsonValue.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/Transport.h>
#include <LibWebView/Forward.h>
#include <WebContent/WebUIClientEndpoint.h>
#include <WebContent/WebUIServerEndpoint.h>

namespace WebView {

class WebUI
    : public IPC::ConnectionToServer<WebUIClientEndpoint, WebUIServerEndpoint>
    , public WebUIClientEndpoint {
public:
    static ErrorOr<RefPtr<WebUI>> create(WebContentClient&, String host);
    virtual ~WebUI();

    String const& host() const { return m_host; }

protected:
    WebUI(WebContentClient&, IPC::Transport, String host);

    using Interface = Function<void(JsonValue)>;

    virtual void register_interfaces() { }
    void register_interface(StringView name, Interface);

private:
    virtual void die() override;
    virtual void received_message(String name, JsonValue data) override;

    WebContentClient& m_client;
    String m_host;

    HashMap<StringView, Interface> m_interfaces;
};

#define WEB_UI(WebUIType)                                                                                   \
public:                                                                                                     \
    static NonnullRefPtr<WebUIType> create(WebContentClient& client, IPC::Transport transport, String host) \
    {                                                                                                       \
        return adopt_ref(*new WebUIType(client, move(transport), move(host)));                              \
    }                                                                                                       \
                                                                                                            \
private:                                                                                                    \
    WebUIType(WebContentClient& client, IPC::Transport transport, String host)                              \
        : WebView::WebUI(client, move(transport), move(host))                                               \
    {                                                                                                       \
    }

}
