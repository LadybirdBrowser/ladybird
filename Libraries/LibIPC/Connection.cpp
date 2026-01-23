/*
 * Copyright (c) 2021-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibIPC/Connection.h>
#include <LibIPC/Message.h>
#include <LibIPC/Stub.h>

namespace IPC {

ConnectionBase::ConnectionBase(IPC::Stub& local_stub, NonnullOwnPtr<Transport> transport, u32 local_endpoint_magic)
    : m_local_stub(local_stub)
    , m_transport(move(transport))
    , m_local_endpoint_magic(local_endpoint_magic)
{
}

void ConnectionBase::initialize_messaging()
{
    m_event_loop = Core::EventLoop::current_weak();

    m_transport->set_message_handler([this](NonnullOwnPtr<IPC::Message> message) {
        on_message_received(move(message));
    });
    m_transport->set_peer_closed_handler([this] {
        on_peer_closed();
    });
}

ConnectionBase::~ConnectionBase()
{
    // Close the transport before destroying member variables (especially the
    // condition variable). This ensures the I/O thread is stopped and joined
    // before we destroy state it may be accessing.
    m_transport->close();
}

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

    TRY(buffer.transfer_message(*m_transport));

    return {};
}

void ConnectionBase::shutdown()
{
    m_transport->close();
    m_unprocessed_messages_cv.broadcast();
    die();
}

void ConnectionBase::shutdown_with_error(Error const& error)
{
    dbgln("IPC::ConnectionBase ({:p}) had an error ({}), disconnecting.", this, error);
    shutdown();
}

void ConnectionBase::on_message_received(NonnullOwnPtr<IPC::Message> message)
{
    // Called from I/O thread - store message and signal waiters
    {
        Threading::MutexLocker lock(m_unprocessed_messages_mutex);
        m_unprocessed_messages.append(move(message));
        m_unprocessed_messages_cv.broadcast();
    }

    // Wake up the main thread's event loop to process messages.
    if (m_event_loop) {
        NonnullRefPtr<ConnectionBase> strong_this = *this;
        m_event_loop->deferred_invoke([strong_this = move(strong_this)] {
            strong_this->handle_messages();
        });
    }
}

void ConnectionBase::on_peer_closed()
{
    m_peer_closed.store(true, AK::MemoryOrder::memory_order_release);
    m_unprocessed_messages_cv.broadcast();

    if (m_event_loop) {
        NonnullRefPtr<ConnectionBase> strong_this = *this;
        m_event_loop->deferred_invoke([strong_this = move(strong_this)] {
            strong_this->shutdown();
        });
    }
}

void ConnectionBase::handle_messages()
{
    Vector<NonnullOwnPtr<Message>> messages;
    {
        Threading::MutexLocker lock(m_unprocessed_messages_mutex);
        messages = move(m_unprocessed_messages);
    }

    for (auto& message : messages) {
        if (message->endpoint_magic() != m_local_endpoint_magic)
            continue;

        if (!is_open())
            dbgln("Handling message while connection closed: {}", message->message_name());

        auto handler_result = m_local_stub.handle(move(message));
        if (handler_result.is_error()) {
            dbgln("IPC::ConnectionBase::handle_messages: {}", handler_result.error());
            continue;
        }

        if (!is_open())
            continue;

        if (auto response = handler_result.release_value()) {
            if (auto post_result = post_message(*response); post_result.is_error())
                dbgln("IPC::ConnectionBase::handle_messages: {}", post_result.error());
        }
    }
}

OwnPtr<IPC::Message> ConnectionBase::wait_for_specific_endpoint_message_impl(u32 endpoint_magic, int message_id)
{
    {
        Threading::MutexLocker lock(m_unprocessed_messages_mutex);
        for (;;) {
            // Double check we don't already have the message waiting for us.
            // Otherwise we might end up blocked for a while for no reason.
            for (size_t i = 0; i < m_unprocessed_messages.size(); ++i) {
                auto& message = m_unprocessed_messages[i];
                if (message->endpoint_magic() != endpoint_magic)
                    continue;
                if (message->message_id() == message_id)
                    return m_unprocessed_messages.take(i);
            }

            if (!is_open() || m_peer_closed.load(AK::MemoryOrder::memory_order_acquire))
                break;

            // Wait for more messages from I/O thread
            m_unprocessed_messages_cv.wait();
        }
    }

    dbgln("Failed to receive message_id: {}", message_id);

    bool should_close_transport = false;
    {
        Threading::MutexLocker lock(m_unprocessed_messages_mutex);
        if (!m_unprocessed_messages.is_empty()) {
            should_close_transport = true;

            dbgln("Transport shutdown with unprocessed messages left: {}", m_unprocessed_messages.size());
            for (size_t i = 0; i < m_unprocessed_messages.size(); ++i) {
                auto& message = m_unprocessed_messages[i];
                dbgln(" Message {:03} is: {:2}-{}", i, message->message_id(), message->message_name());
            }
        }
    }
    if (should_close_transport)
        m_transport->close();

    dbgln("Handling remaining messages before returning to caller");
    handle_messages();
    dbgln("Messages handled, returning to caller");

    return {};
}

}
