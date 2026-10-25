/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/Socket.h>
#include <LibWebSocket/Impl/WebSocketImpl.h>

namespace WebSocket {

// A connected TCP stream accepted by a server. The WebSocket layer performs the HTTP upgrade itself.
class AcceptedSocket final : public WebSocketImpl {
public:
    static ErrorOr<NonnullRefPtr<AcceptedSocket>> create(NonnullOwnPtr<Core::TCPSocket>);
    virtual ~AcceptedSocket() override;

    virtual void connect(ConnectionInfo const&) override;
    virtual bool can_read_line() override { return false; }
    virtual ErrorOr<ByteString> read_line(size_t) override { return Error::from_errno(ENOTSUP); }
    virtual ErrorOr<ByteBuffer> read(int max_size) override;
    virtual bool send(ReadonlyBytes) override;
    virtual bool eof() override;
    virtual void discard_connection() override;

private:
    explicit AcceptedSocket(NonnullOwnPtr<Core::TCPSocket>);
    NonnullOwnPtr<Core::TCPSocket> m_socket;
};

}
