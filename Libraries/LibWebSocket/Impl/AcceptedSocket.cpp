/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebSocket/Impl/AcceptedSocket.h>

namespace WebSocket {

ErrorOr<NonnullRefPtr<AcceptedSocket>> AcceptedSocket::create(NonnullOwnPtr<Core::TCPSocket> socket)
{
    TRY(socket->set_blocking(true));
    return adopt_nonnull_ref_or_enomem(new (nothrow) AcceptedSocket(move(socket)));
}

AcceptedSocket::AcceptedSocket(NonnullOwnPtr<Core::TCPSocket> socket)
    : m_socket(move(socket))
{
    m_socket->on_ready_to_read = [this] {
        if (on_ready_to_read)
            on_ready_to_read();
    };
}

AcceptedSocket::~AcceptedSocket()
{
    discard_connection();
}

void AcceptedSocket::connect(ConnectionInfo const&)
{
    if (on_connected)
        on_connected();
}

ErrorOr<ByteBuffer> AcceptedSocket::read(int max_size)
{
    if (!TRY(m_socket->can_read_without_blocking()))
        return ByteBuffer {};
    auto buffer = TRY(ByteBuffer::create_uninitialized(max_size));
    auto bytes = TRY(m_socket->read_some(buffer));
    buffer.trim(bytes.size(), false);
    return buffer;
}

bool AcceptedSocket::send(ReadonlyBytes bytes)
{
    return !m_socket->write_until_depleted(bytes).is_error();
}

bool AcceptedSocket::eof()
{
    return m_socket->is_eof();
}

void AcceptedSocket::discard_connection()
{
    m_socket->on_ready_to_read = nullptr;
    m_socket->close();
}

}
