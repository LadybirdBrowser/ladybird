/*
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebSockets/WebSocket.h>
#include <LibWebSocket/WebSocket.h>

namespace Ladybird {

class WebSocketQt
    : public Web::WebSockets::WebSocketClientSocket
    , public Weakable<WebSocketQt> {
public:
    static NonnullRefPtr<WebSocketQt> create(NonnullRefPtr<WebSocket::WebSocket>);

    virtual ~WebSocketQt() override;

    virtual Web::WebSockets::WebSocket::ReadyState ready_state() override;
    virtual ByteString subprotocol_in_use() override;
    virtual void send(ByteBuffer binary_or_text_message, bool is_text) override;
    virtual void send(StringView message) override;
    virtual void close(u16 code, ByteString reason) override;

private:
    explicit WebSocketQt(NonnullRefPtr<WebSocket::WebSocket>);

    NonnullRefPtr<WebSocket::WebSocket> m_websocket;
};

}
