/*
 * Copyright (c) 2021, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/RefCounted.h>
#include <AK/WeakPtr.h>

namespace Requests {

class RequestClient;

class WebSocket : public RefCounted<WebSocket> {
public:
    struct CertificateAndKey {
        ByteString certificate;
        ByteString key;
    };

    struct Message {
        ByteBuffer data;
        bool is_text { false };
    };

    enum class Error {
        CouldNotEstablishConnection,
        ConnectionUpgradeFailed,
        ServerClosedSocket,
    };

    enum class ReadyState {
        Connecting = 0,
        Open = 1,
        Closing = 2,
        Closed = 3,
    };

    static NonnullRefPtr<WebSocket> create_from_id(Badge<RequestClient>, RequestClient& client, i64 websocket_id)
    {
        return adopt_ref(*new WebSocket(client, websocket_id));
    }

    i64 id() const { return m_websocket_id; }

    ReadyState ready_state();
    void set_ready_state(ReadyState);

    ByteString subprotocol_in_use();
    void set_subprotocol_in_use(ByteString);

    void send(ByteBuffer const& binary_or_text_message, bool is_text);
    void send(StringView text_message);
    void close(u16 code = 1005, ByteString const& reason = {});

    Function<void()> on_open;
    Function<void(Message)> on_message;
    Function<void(Error)> on_error;
    Function<void(u16 code, ByteString reason, bool was_clean)> on_close;
    Function<CertificateAndKey()> on_certificate_requested;

    void did_open(Badge<RequestClient>);
    void did_receive(Badge<RequestClient>, ByteBuffer, bool);
    void did_error(Badge<RequestClient>, i32);
    void did_close(Badge<RequestClient>, u16, ByteString, bool);
    void did_request_certificates(Badge<RequestClient>);

private:
    explicit WebSocket(RequestClient&, i64 websocket_id);
    WeakPtr<RequestClient> m_client;
    ReadyState m_ready_state { ReadyState::Connecting };
    ByteString m_subprotocol;
    i64 m_websocket_id { -1 };
};

}
