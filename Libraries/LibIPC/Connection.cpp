/*
 * Copyright (c) 2021-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibCore/Socket.h>
#include <LibCore/Timer.h>
#include <LibIPC/Connection.h>
#include <LibIPC/Message.h>
#include <LibIPC/Stub.h>

namespace IPC {

ConnectionBase::ConnectionBase(IPC::Stub& local_stub, NonnullOwnPtr<Transport> transport, u32 local_endpoint_magic)
    : m_local_stub(local_stub)
    , m_transport(move(transport))
    , m_local_endpoint_magic(local_endpoint_magic)
{
    m_responsiveness_timer = Core::Timer::create_single_shot(3000, [this] { may_have_become_unresponsive(); });

    m_transport->set_up_read_hook([this] {
        NonnullRefPtr protect = *this;
        // FIXME: Do something about errors.
        (void)drain_messages_from_peer();
        handle_messages();
    });
}

ConnectionBase::~ConnectionBase() = default;

bool ConnectionBase::is_open() const
{
    return m_transport->is_open();
}

ErrorOr<void> ConnectionBase::post_message(Message const& message)
{
    return post_message(TRY(message.encode()));
}

ErrorOr<void> ConnectionBase::post_message(MessageBuffer buffer)
{
    // NOTE: If this connection is being shut down, but has not yet been destroyed,
    //       the socket will be closed. Don't try to send more messages.
    if (!m_transport->is_open())
        return Error::from_string_literal("Trying to post_message during IPC shutdown");

    MUST(buffer.transfer_message(*m_transport));

    m_responsiveness_timer->start();
    return {};
}

void ConnectionBase::shutdown()
{
    m_transport->close();
    die();
}

void ConnectionBase::shutdown_with_error(Error const& error)
{
    dbgln("IPC::ConnectionBase ({:p}) had an error ({}), disconnecting.", this, error);
    shutdown();
}

void ConnectionBase::handle_messages()
{
    auto messages = move(m_unprocessed_messages);
    for (auto& message : messages) {
        if (message->endpoint_magic() == m_local_endpoint_magic) {
            auto handler_result = m_local_stub.handle(move(message));
            if (handler_result.is_error()) {
                dbgln("IPC::ConnectionBase::handle_messages: {}", handler_result.error());
                continue;
            }

            if (auto response = handler_result.release_value()) {
                if (auto post_result = post_message(*response); post_result.is_error()) {
                    dbgln("IPC::ConnectionBase::handle_messages: {}", post_result.error());
                }
            }
        }
    }
}

void ConnectionBase::wait_for_transport_to_become_readable()
{
    m_transport->wait_until_readable();
}

ErrorOr<void> ConnectionBase::drain_messages_from_peer()
{
    auto schedule_shutdown = m_transport->read_as_many_messages_as_possible_without_blocking([&](auto&& raw_message) {
        if (auto message = try_parse_message(raw_message.bytes, raw_message.fds)) {
            m_unprocessed_messages.append(message.release_nonnull());
        } else {
            dbgln("Failed to parse IPC message {:hex-dump}", raw_message.bytes);
            VERIFY_NOT_REACHED();
        }
    });

    if (!m_unprocessed_messages.is_empty()) {
        m_responsiveness_timer->stop();
        did_become_responsive();
        deferred_invoke([this] {
            handle_messages();
        });
    }

    if (schedule_shutdown == TransportSocket::ShouldShutdown::Yes) {
        deferred_invoke([this] {
            shutdown();
        });
        return Error::from_string_literal("IPC connection EOF");
    }

    return {};
}

OwnPtr<IPC::Message> ConnectionBase::wait_for_specific_endpoint_message_impl(u32 endpoint_magic, int message_id)
{
    for (;;) {
        // Double check we don't already have the event waiting for us.
        // Otherwise we might end up blocked for a while for no reason.
        for (size_t i = 0; i < m_unprocessed_messages.size(); ++i) {
            auto& message = m_unprocessed_messages[i];
            if (message->endpoint_magic() != endpoint_magic)
                continue;
            if (message->message_id() == message_id)
                return m_unprocessed_messages.take(i);
        }

        if (!is_open())
            break;

        wait_for_transport_to_become_readable();
        if (drain_messages_from_peer().is_error())
            break;
    }
    return {};
}

}
