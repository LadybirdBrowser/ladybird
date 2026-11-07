/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/Debug.h>
#include <AK/GenericShorthands.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <LibCore/EventLoop.h>
#include <LibDevTools/Connection.h>
#include <errno.h>

namespace DevTools {

NonnullRefPtr<Connection> Connection::create(NonnullOwnPtr<Core::TCPSocket> socket)
{
    return adopt_ref(*new Connection(move(socket)));
}

Connection::Connection(NonnullOwnPtr<Core::TCPSocket> socket)
    : m_socket(move(socket))
    , m_write_notifier(Core::Notifier::construct(m_socket->fd(), Core::Notifier::Type::Write))
{
    (void)m_socket->set_blocking(false);

    m_socket->on_ready_to_read = [this]() {
        if (auto result = on_ready_to_read(); result.is_error()) {
            if (on_connection_closed)
                on_connection_closed();
        }
    };

    m_write_notifier->set_enabled(false);
    m_write_notifier->on_activation = [this]() {
        flush_outgoing_packets();
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

    auto was_idle = m_outgoing_packets.is_empty();
    m_outgoing_packets.enqueue(MUST(String::formatted("{}:{}", serialized.byte_count(), serialized)));

    if (was_idle)
        flush_outgoing_packets();
}

void Connection::flush_outgoing_packets()
{
    while (!m_outgoing_packets.is_empty()) {
        auto packet = m_outgoing_packets.head().bytes().slice(m_outgoing_packet_offset);

        auto result = m_socket->write_some(packet);
        if (result.is_error()) {
            auto const& error = result.error();
            if (error.is_errno() && first_is_one_of(error.code(), EAGAIN, EWOULDBLOCK, EINTR)) {
                m_write_notifier->set_enabled(true);
                return;
            }

            warnln("DevTools: Failed to send message ({} bytes): {}", m_outgoing_packets.head().byte_count(), error);
            m_outgoing_packets.clear();
            m_outgoing_packet_offset = 0;
            m_write_notifier->set_enabled(false);

            if (on_connection_closed)
                on_connection_closed();
            return;
        }

        m_outgoing_packet_offset += result.value();
        if (m_outgoing_packet_offset == m_outgoing_packets.head().byte_count()) {
            (void)m_outgoing_packets.dequeue();
            m_outgoing_packet_offset = 0;
        }
    }

    m_write_notifier->set_enabled(false);
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
    if (Checked<size_t>::addition_would_overflow(message_offset, *length))
        return Error::from_string_literal("DevTools message length is too large");

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
