/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/StringView.h>
#include <AK/Weakable.h>
#include <LibCore/Forward.h>
#include <LibCore/Socket.h>
#include <LibCore/TCPServer.h>
#include <LibWebSocket/WebSocket.h>

namespace WebDriver {

// The server side of a WebSocket connection, https://www.rfc-editor.org/rfc/rfc6455
class WebSocketConnection
    : public RefCounted<WebSocketConnection>
    , public Weakable<WebSocketConnection> {
public:
    static ErrorOr<NonnullRefPtr<WebSocketConnection>> create(NonnullOwnPtr<Core::TCPSocket>, u16 listener_port);
    ~WebSocketConnection();

    enum class MessageType : u8 {
        Text,
        Binary,
    };

    // Decides whether to accept the connection, given the resource name from the client's opening handshake.
    Function<bool(StringView resource_name)> on_opening_handshake;
    Function<void(MessageType, ByteBuffer)> on_message;
    Function<void()> on_close;

    bool is_open() const { return m_websocket->ready_state() == WebSocket::ReadyState::Open; }

    void send_text_message(StringView);

    // https://www.rfc-editor.org/rfc/rfc6455#section-7.1.2
    void start_the_closing_handshake();

private:
    WebSocketConnection(NonnullRefPtr<WebSocket::WebSocket>, u16 listener_port);

    NonnullRefPtr<WebSocket::WebSocket> m_websocket;
    u16 m_listener_port { 0 };
};

// https://w3c.github.io/webdriver-bidi/#websocket-listener
class WebSocketListener {
    AK_MAKE_NONCOPYABLE(WebSocketListener);
    AK_MAKE_NONMOVABLE(WebSocketListener);
    AK_ALLOC_WITH_KMALLOC;

public:
    // NB: The listener only accepts connections from the local machine.
    static ErrorOr<NonnullOwnPtr<WebSocketListener>> create();

    u16 port() const { return m_port; }

    Function<void(NonnullRefPtr<WebSocketConnection>)> on_connection;

private:
    WebSocketListener(NonnullRefPtr<Core::TCPServer>, u16 port);

    NonnullRefPtr<Core::TCPServer> m_server;
    u16 m_port { 0 };
};

}
