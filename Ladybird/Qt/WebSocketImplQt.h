/*
 * Copyright (c) 2021, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebSocket/Impl/WebSocketImpl.h>

class QTcpSocket;

namespace Ladybird {

class WebSocketImplQt final : public WebSocket::WebSocketImpl {
public:
    explicit WebSocketImplQt();
    virtual ~WebSocketImplQt() override;

    virtual void connect(WebSocket::ConnectionInfo const&) override;
    virtual bool can_read_line() override;
    virtual ErrorOr<ByteString> read_line(size_t) override;
    virtual ErrorOr<ByteBuffer> read(int max_size) override;
    virtual bool send(ReadonlyBytes) override;
    virtual bool eof() override;
    virtual void discard_connection() override;

private:
    OwnPtr<QTcpSocket> m_socket;
};

}
