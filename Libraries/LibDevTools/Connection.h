/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Queue.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <AK/Weakable.h>
#include <LibCore/Notifier.h>
#include <LibCore/Socket.h>
#include <LibDevTools/Forward.h>

namespace DevTools {

class DEVTOOLS_API Connection
    : public RefCounted<Connection>
    , public Weakable<Connection> {
public:
    static NonnullRefPtr<Connection> create(NonnullOwnPtr<Core::TCPSocket>);
    ~Connection();

    Function<void()> on_connection_closed;
    Function<void(JsonObject)> on_message_received;

    void send_message(JsonValue const&);

private:
    explicit Connection(NonnullOwnPtr<Core::TCPSocket>);

    ErrorOr<void> on_ready_to_read();
    ErrorOr<void> read_available_data();
    ErrorOr<Optional<JsonValue>> read_message();

    void flush_outgoing_packets();

    NonnullOwnPtr<Core::TCPSocket> m_socket;
    NonnullRefPtr<Core::Notifier> m_write_notifier;
    ByteBuffer m_incoming_buffer;

    Queue<String> m_outgoing_packets;
    size_t m_outgoing_packet_offset { 0 };
};

}
