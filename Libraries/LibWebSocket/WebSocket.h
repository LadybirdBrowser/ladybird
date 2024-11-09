/*
 * Copyright (c) 2021, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <LibCore/EventReceiver.h>
#include <LibWebSocket/ConnectionInfo.h>
#include <LibWebSocket/Impl/WebSocketImpl.h>
#include <LibWebSocket/Message.h>

namespace WebSocket {

enum class ReadyState {
    Connecting = 0,
    Open = 1,
    Closing = 2,
    Closed = 3,
};

// https://datatracker.ietf.org/doc/html/rfc6455#section-7.4.1
enum class CloseStatusCode : u16 {
    Normal = 1000,
    GoingAway = 1001,
    ProtocolError = 1002,
    UnsupportedData = 1003,
    AbnormalClosure = 1006,
    InvalidPayload = 1007,
    PolicyViolation = 1008,
    MessageTooBig = 1009,
    MissingExtension = 1010,
    UnexpectedCondition = 1011,
};

class WebSocket final : public Core::EventReceiver {
    C_OBJECT(WebSocket)
public:
    static NonnullRefPtr<WebSocket> create(ConnectionInfo, RefPtr<WebSocketImpl> = nullptr);
    virtual ~WebSocket() override = default;

    URL::URL const& url() const { return m_connection.url(); }

    ReadyState ready_state();

    ByteString subprotocol_in_use();

    // Call this to start the WebSocket connection.
    void start();

    // This can only be used if the `ready_state` is `ReadyState::Open`
    void send(Message const&);

    // This can only be used if the `ready_state` is `ReadyState::Open`
    void close(u16 code = 1005, ByteString const& reason = {});

    Function<void()> on_open;
    Function<void(u16 code, ByteString reason, bool was_clean)> on_close;
    Function<void(Message message)> on_message;
    Function<void(ReadyState)> on_ready_state_change;
    Function<void(ByteString)> on_subprotocol;

    enum class Error {
        CouldNotEstablishConnection,
        ConnectionUpgradeFailed,
        ServerClosedSocket,
    };

    Function<void(Error)> on_error;

private:
    WebSocket(ConnectionInfo, RefPtr<WebSocketImpl>);

    // As defined in section 5.2
    enum class OpCode : u8 {
        Continuation = 0x0,
        Text = 0x1,
        Binary = 0x2,
        ConnectionClose = 0x8,
        Ping = 0x9,
        Pong = 0xA,
    };

    void drain_read();

    void send_client_handshake();
    void read_server_handshake();

    void read_frame();
    void send_frame(OpCode, ReadonlyBytes, bool is_final);

    void notify_open();
    void notify_close(u16 code, ByteString reason, bool was_clean);
    void notify_error(Error);
    void notify_message(Message);

    void fatal_error(Error);
    void discard_connection();

    enum class InternalState {
        NotStarted,
        EstablishingProtocolConnection,
        SendingClientHandshake,
        WaitingForServerHandshake,
        Open,
        Closing,
        Closed,
        Errored,
    };

    InternalState m_state { InternalState::NotStarted };

    void set_state(InternalState);

    void fail_connection(u16 close_status_code, WebSocket::Error, ByteString const& reason);

    ByteString m_subprotocol_in_use { ByteString::empty() };

    ByteString m_websocket_key;
    bool m_has_read_server_handshake_first_line { false };
    bool m_has_read_server_handshake_upgrade { false };
    bool m_has_read_server_handshake_connection { false };
    bool m_has_read_server_handshake_accept { false };

    bool m_discard_connection_requested { false };

    u16 m_last_close_code { 1005 };
    ByteString m_last_close_message;

    ConnectionInfo m_connection;
    RefPtr<WebSocketImpl> m_impl;

    Vector<u8> m_buffered_data;
    ByteBuffer m_fragmented_data_buffer;
    WebSocket::OpCode m_initial_fragment_opcode;
};

}
