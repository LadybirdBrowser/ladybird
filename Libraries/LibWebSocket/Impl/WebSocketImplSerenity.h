/*
 * Copyright (c) 2021, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebSocket/Impl/WebSocketImpl.h>

namespace WebSocket {

class WebSocketImplSerenity final : public WebSocketImpl {
public:
    explicit WebSocketImplSerenity();
    virtual ~WebSocketImplSerenity() override;

    virtual void connect(ConnectionInfo const&) override;
    virtual bool can_read_line() override;
    virtual ErrorOr<ByteString> read_line(size_t) override;
    virtual ErrorOr<ByteBuffer> read(int max_size) override;
    virtual bool send(ReadonlyBytes) override;
    virtual bool eof() override;
    virtual void discard_connection() override;

private:
    OwnPtr<Core::BufferedSocketBase> m_socket;
};

}
