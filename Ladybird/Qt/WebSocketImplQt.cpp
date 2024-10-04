/*
 * Copyright (c) 2021, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WebSocketImplQt.h"
#include "StringUtils.h"
#include <LibCore/EventLoop.h>
#include <QSslSocket>
#include <QTcpSocket>

namespace Ladybird {

WebSocketImplQt::~WebSocketImplQt() = default;
WebSocketImplQt::WebSocketImplQt() = default;

bool WebSocketImplQt::can_read_line()
{
    return m_socket->canReadLine();
}

bool WebSocketImplQt::send(ReadonlyBytes bytes)
{
    auto bytes_written = m_socket->write(reinterpret_cast<char const*>(bytes.data()), bytes.size());
    if (bytes_written == -1)
        return false;
    VERIFY(static_cast<size_t>(bytes_written) == bytes.size());
    return true;
}

bool WebSocketImplQt::eof()
{
    return m_socket->state() == QTcpSocket::SocketState::UnconnectedState
        && !m_socket->bytesAvailable();
}

void WebSocketImplQt::discard_connection()
{
    m_socket = nullptr;
}

void WebSocketImplQt::connect(WebSocket::ConnectionInfo const& connection_info)
{
    VERIFY(!m_socket);
    VERIFY(on_connected);
    VERIFY(on_connection_error);
    VERIFY(on_ready_to_read);

    if (connection_info.is_secure()) {
        auto ssl_socket = make<QSslSocket>();
        ssl_socket->connectToHostEncrypted(
            qstring_from_ak_string(MUST(connection_info.url().serialized_host())),
            connection_info.url().port_or_default());
        QObject::connect(ssl_socket.ptr(), &QSslSocket::alertReceived, [this](QSsl::AlertLevel level, QSsl::AlertType, QString const&) {
            if (level == QSsl::AlertLevel::Fatal)
                on_connection_error();
        });
        m_socket = move(ssl_socket);
    } else {
        m_socket = make<QTcpSocket>();
        m_socket->connectToHost(
            qstring_from_ak_string(MUST(connection_info.url().serialized_host())),
            connection_info.url().port_or_default());
    }

    QObject::connect(m_socket.ptr(), &QTcpSocket::readyRead, [this] {
        on_ready_to_read();
    });

    QObject::connect(m_socket.ptr(), &QTcpSocket::connected, [this] {
        on_connected();
    });
}

ErrorOr<ByteBuffer> WebSocketImplQt::read(int max_size)
{
    auto buffer = TRY(ByteBuffer::create_uninitialized(max_size));
    auto bytes_read = m_socket->read(reinterpret_cast<char*>(buffer.data()), buffer.size());
    if (bytes_read == -1)
        return Error::from_string_literal("WebSocketImplQt::read(): Error reading from socket");
    return buffer.slice(0, bytes_read);
}

ErrorOr<ByteString> WebSocketImplQt::read_line(size_t size)
{
    auto buffer = TRY(ByteBuffer::create_uninitialized(size));
    auto bytes_read = m_socket->readLine(reinterpret_cast<char*>(buffer.data()), buffer.size());
    if (bytes_read == -1)
        return Error::from_string_literal("WebSocketImplQt::read_line(): Error reading from socket");
    return ByteString::copy(buffer.span().slice(0, bytes_read));
}

}
