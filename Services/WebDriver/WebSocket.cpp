/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IPv4Address.h>
#include <LibCore/EventLoop.h>
#include <WebDriver/WebSocket.h>

namespace WebDriver {

// NB: A WebDriver client connects to the address the listener returned in the webSocketUrl capability. A request naming
//     another host was not meant for us, and could come from a page using DNS rebinding.
static bool is_host_of_local_listener(StringView host, u16 port)
{
    auto colon = host.find_last(':');
    if (!colon.has_value())
        return false;

    auto port_string = host.substring_view(*colon + 1);
    if (port_string.to_number<u16>() != port)
        return false;

    auto name = host.substring_view(0, *colon);
    return name == "127.0.0.1"sv || name == "[::1]"sv || name.equals_ignoring_ascii_case("localhost"sv);
}

ErrorOr<NonnullRefPtr<WebSocketConnection>> WebSocketConnection::create(NonnullOwnPtr<Core::TCPSocket> socket, u16 listener_port)
{
    auto websocket = TRY(WebSocket::WebSocket::accept(move(socket), {}));
    auto connection = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) WebSocketConnection(move(websocket), listener_port)));
    connection->m_websocket->start();
    return connection;
}

WebSocketConnection::WebSocketConnection(NonnullRefPtr<WebSocket::WebSocket> websocket, u16 listener_port)
    : m_websocket(move(websocket))
    , m_listener_port(listener_port)
{
    m_websocket->on_client_handshake = [this](auto const& request) -> Optional<ByteString> {
        auto protector = NonnullRefPtr { *this };
        auto host = request.headers->get("Host"sv);
        if (!host.has_value() || !is_host_of_local_listener(*host, m_listener_port))
            return "403 Forbidden";

        // NB: A browser page sends Origin, while a WebDriver client does not. Reject page access to automation.
        if (request.headers->contains("Origin"sv))
            return "403 Forbidden";
        if (!on_opening_handshake || !on_opening_handshake(request.resource_name))
            return "404 Not Found";
        return {};
    };
    m_websocket->on_message = [this](WebSocket::Message message) {
        auto protector = NonnullRefPtr { *this };
        if (on_message)
            on_message(message.is_text() ? MessageType::Text : MessageType::Binary, message.release_data());
    };
    m_websocket->on_close = [this](auto, auto, auto) {
        // NB: Session callbacks may drop the connection. Keep them outside the socket's read handler.
        Core::deferred_invoke([connection = NonnullRefPtr { *this }] {
            if (auto on_close = move(connection->on_close))
                on_close();
        });
    };
}

WebSocketConnection::~WebSocketConnection()
{
    m_websocket->on_client_handshake = nullptr;
    m_websocket->on_message = nullptr;
    m_websocket->on_close = nullptr;
}

void WebSocketConnection::send_text_message(StringView message)
{
    if (is_open())
        m_websocket->send(WebSocket::Message(message.to_byte_string()));
}

void WebSocketConnection::start_the_closing_handshake()
{
    m_websocket->close(to_underlying(WebSocket::CloseStatusCode::Normal));
}

ErrorOr<NonnullOwnPtr<WebSocketListener>> WebSocketListener::create()
{
    auto server = TRY(Core::TCPServer::try_create());

    // NB: An ephemeral port on the loopback interface.
    TRY(server->listen(IPv4Address { 127, 0, 0, 1 }, 0));

    auto port = server->local_port();
    if (!port.has_value())
        return Error::from_string_literal("Unable to determine the port of the WebSocket listener");

    return adopt_nonnull_own_or_enomem(new (nothrow) WebSocketListener(move(server), *port));
}

WebSocketListener::WebSocketListener(NonnullRefPtr<Core::TCPServer> server, u16 port)
    : m_server(move(server))
    , m_port(port)
{
    m_server->on_ready_to_accept = [this] {
        auto socket = m_server->accept();
        if (socket.is_error()) {
            warnln("Failed to accept a WebSocket connection: {}", socket.error());
            return;
        }

        auto connection = WebSocketConnection::create(socket.release_value(), m_port);
        if (connection.is_error()) {
            warnln("Failed to create a WebSocket connection: {}", connection.error());
            return;
        }

        if (on_connection)
            on_connection(connection.release_value());
    };
}

}
