/*
 * Copyright (c) 2021, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Span.h>
#include <LibWebSocket/ConnectionInfo.h>

namespace WebSocket {

class WebSocketImpl : public RefCounted<WebSocketImpl> {
public:
    virtual ~WebSocketImpl();

    virtual void connect(ConnectionInfo const&) = 0;
    virtual ErrorOr<ByteBuffer> read(int max_size) = 0;
    virtual bool send(ReadonlyBytes) = 0;
    virtual bool eof() = 0;
    virtual void discard_connection() = 0;

    Function<void()> on_connected;
    Function<void()> on_connection_error;
    Function<void()> on_ready_to_read;

protected:
    WebSocketImpl();
};

}
