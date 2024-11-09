/*
 * Copyright (c) 2021, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibWebSocket/Impl/WebSocketImplSerenity.h>

namespace WebSocket {

WebSocketImplSerenity::~WebSocketImplSerenity() = default;
WebSocketImplSerenity::WebSocketImplSerenity() = default;

bool WebSocketImplSerenity::can_read_line()
{
    return MUST(m_socket->can_read_line());
}

bool WebSocketImplSerenity::send(ReadonlyBytes bytes)
{
    return !m_socket->write_until_depleted(bytes).is_error();
}

bool WebSocketImplSerenity::eof()
{
    return m_socket->is_eof();
}

void WebSocketImplSerenity::discard_connection()
{
    m_socket = nullptr;
}

void WebSocketImplSerenity::connect(ConnectionInfo const& connection_info)
{
    VERIFY(!m_socket);
    VERIFY(on_connected);
    VERIFY(on_connection_error);
    VERIFY(on_ready_to_read);
    auto socket_result = [&]() -> ErrorOr<NonnullOwnPtr<Core::BufferedSocketBase>> {
        auto host = TRY(connection_info.url().serialized_host()).to_byte_string();
        if (connection_info.is_secure()) {
            TLS::Options options;
            options.set_alert_handler([this](auto) {
                on_connection_error();
            });

            return TRY(Core::BufferedSocket<TLS::TLSv12>::create(
                TRY(TLS::TLSv12::connect(host, connection_info.url().port_or_default(), move(options)))));
        }

        return TRY(Core::BufferedTCPSocket::create(
            TRY(Core::TCPSocket::connect(host, connection_info.url().port_or_default()))));
    }();

    if (socket_result.is_error()) {
        Core::deferred_invoke([this] {
            on_connection_error();
        });
        return;
    }

    m_socket = socket_result.release_value();

    m_socket->on_ready_to_read = [this] {
        on_ready_to_read();
    };

    Core::deferred_invoke([this] {
        on_connected();
    });
}

ErrorOr<ByteBuffer> WebSocketImplSerenity::read(int max_size)
{
    auto buffer = TRY(ByteBuffer::create_uninitialized(max_size));
    auto read_bytes = TRY(m_socket->read_some(buffer));
    return buffer.slice(0, read_bytes.size());
}

ErrorOr<ByteString> WebSocketImplSerenity::read_line(size_t size)
{
    auto buffer = TRY(ByteBuffer::create_uninitialized(size));
    auto line = TRY(m_socket->read_line(buffer));
    return line.to_byte_string();
}

}
