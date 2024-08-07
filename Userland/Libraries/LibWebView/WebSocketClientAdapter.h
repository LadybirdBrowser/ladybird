/*
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Weakable.h>
#include <LibWeb/WebSockets/WebSocket.h>

namespace Requests {
class WebSocket;
class RequestClient;
};

namespace WebView {

class WebSocketClientSocketAdapter
    : public Web::WebSockets::WebSocketClientSocket
    , public Weakable<WebSocketClientSocketAdapter> {
public:
    static RefPtr<WebSocketClientSocketAdapter> create(NonnullRefPtr<Requests::WebSocket>);
    virtual ~WebSocketClientSocketAdapter() override;

    virtual Web::WebSockets::WebSocket::ReadyState ready_state() override;
    virtual ByteString subprotocol_in_use() override;

    virtual void send(ByteBuffer binary_or_text_message, bool is_text) override;
    virtual void send(StringView text_message) override;
    virtual void close(u16 code = 1005, ByteString reason = {}) override;

private:
    WebSocketClientSocketAdapter(NonnullRefPtr<Requests::WebSocket>);

    NonnullRefPtr<Requests::WebSocket> m_websocket;
};

}
