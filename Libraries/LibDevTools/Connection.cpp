/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <LibCore/EventLoop.h>
#include <LibDevTools/Connection.h>

namespace DevTools {

NonnullRefPtr<Connection> Connection::create(NonnullOwnPtr<Core::BufferedTCPSocket> socket)
{
    return adopt_ref(*new Connection(move(socket)));
}

Connection::Connection(NonnullOwnPtr<Core::BufferedTCPSocket> socket)
    : m_socket(move(socket))
{
    m_socket->on_ready_to_read = [this]() {
        if (auto result = on_ready_to_read(); result.is_error()) {
            if (on_connection_closed)
                on_connection_closed();
        }
    };
}

Connection::~Connection() = default;

// https://firefox-source-docs.mozilla.org/devtools/backend/protocol.html#packets
void Connection::send_message(JsonValue const& message)
{
    auto serialized = message.serialized();

    if constexpr (DEVTOOLS_DEBUG) {
        if (message.is_object() && message.as_object().get("error"sv).has_value())
            dbgln("\x1b[1;31m<<\x1b[0m {}", serialized);
        else
            dbgln("\x1b[1;32m<<\x1b[0m {}", serialized);
    }

    if (m_socket->write_formatted("{}:{}", serialized.byte_count(), serialized).is_error()) {
        if (on_connection_closed)
            on_connection_closed();
    }
}

// https://firefox-source-docs.mozilla.org/devtools/backend/protocol.html#packets
ErrorOr<JsonValue> Connection::read_message()
{
    ByteBuffer length_buffer;

    // FIXME: `read_until(':')` would be nicer here, but that seems to return immediately without receiving any data.
    while (true) {
        auto byte = TRY(m_socket->read_value<u8>());
        if (byte == ':') {
            break;
        }

        length_buffer.append(byte);
    }

    auto length = StringView { length_buffer }.to_number<size_t>();
    if (!length.has_value())
        return Error::from_string_literal("Could not read message length from DevTools client");

    ByteBuffer message_buffer;
    message_buffer.resize(*length);

    TRY(m_socket->read_until_filled(message_buffer));

    auto message = TRY(JsonValue::from_string(message_buffer));
    dbgln_if(DEVTOOLS_DEBUG, "\x1b[1;33m>>\x1b[0m {}", message);

    return message;
}

ErrorOr<void> Connection::on_ready_to_read()
{
    // https://firefox-source-docs.mozilla.org/devtools/backend/protocol.html#the-request-reply-pattern
    // Note that it is correct for a client to send several requests to a request/reply actor without waiting for a
    // reply to each request before sending the next; requests can be pipelined.
    while (TRY(m_socket->can_read_without_blocking())) {
        auto message = TRY(read_message());
        if (!message.is_object())
            continue;

        Core::deferred_invoke([this, message = move(message)]() {
            if (on_message_received)
                on_message_received(message.as_object());
        });
    }

    return {};
}

}
