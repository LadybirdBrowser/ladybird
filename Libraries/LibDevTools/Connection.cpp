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
    (void)m_socket->set_blocking(false);

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

    // Temporarily enable blocking mode for large writes to avoid EAGAIN
    (void)m_socket->set_blocking(true);
    auto result = m_socket->write_until_depleted(MUST(String::formatted("{}:{}", serialized.byte_count(), serialized)));
    (void)m_socket->set_blocking(false);

    if (result.is_error()) {
        warnln("DevTools: Failed to send message ({} bytes): {}", serialized.byte_count(), result.error());
        if (on_connection_closed)
            on_connection_closed();
    }
}

ErrorOr<void> Connection::read_available_data()
{
    auto buffer = TRY(ByteBuffer::create_uninitialized(4096));

    while (TRY(m_socket->can_read_without_blocking())) {
        auto bytes = TRY(m_socket->read_some(buffer));
        if (bytes.is_empty()) {
            if (m_socket->is_eof())
                return Error::from_string_literal("DevTools client disconnected");
            break;
        }

        TRY(m_incoming_buffer.try_append(bytes));
    }

    return {};
}

// https://firefox-source-docs.mozilla.org/devtools/backend/protocol.html#packets
ErrorOr<Optional<JsonValue>> Connection::read_message()
{
    auto const packet = StringView { m_incoming_buffer };
    auto colon_offset = packet.find(':');
    if (!colon_offset.has_value())
        return Optional<JsonValue> {};

    auto length = packet.substring_view(0, *colon_offset).to_number<size_t>();
    if (!length.has_value())
        return Error::from_string_literal("Could not read message length from DevTools client");

    auto const message_offset = *colon_offset + 1;
    auto const packet_size = message_offset + *length;
    if (m_incoming_buffer.size() < packet_size)
        return Optional<JsonValue> {};

    auto message = TRY(JsonValue::from_string(packet.substring_view(message_offset, *length)));

    dbgln_if(DEVTOOLS_DEBUG, "\x1b[1;33m>>\x1b[0m {}", message);

    if (packet_size == m_incoming_buffer.size()) {
        m_incoming_buffer.clear();
    } else {
        m_incoming_buffer = TRY(m_incoming_buffer.slice(packet_size, m_incoming_buffer.size() - packet_size));
    }

    return message;
}

ErrorOr<void> Connection::on_ready_to_read()
{
    TRY(read_available_data());

    // https://firefox-source-docs.mozilla.org/devtools/backend/protocol.html#the-request-reply-pattern
    // Note that it is correct for a client to send several requests to a request/reply actor without waiting for a
    // reply to each request before sending the next; requests can be pipelined.
    while (true) {
        auto message = TRY(read_message());
        if (!message.has_value())
            break;

        auto value = message.release_value();
        if (!value.is_object())
            continue;

        Core::deferred_invoke([weak_self = make_weak_ptr<Connection>(), message = move(value)]() mutable {
            auto self = weak_self.strong_ref();
            if (!self)
                return;

            if (self->on_message_received)
                self->on_message_received(move(message.as_object()));
        });
    }

    return {};
}

}
