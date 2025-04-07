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
#include <LibIPC/UnprocessedFileDescriptors.h>

namespace IPC {

ConnectionBase::ConnectionBase(IPC::Stub& local_stub, Transport transport, u32 local_endpoint_magic)
    : m_local_stub(local_stub)
    , m_transport(move(transport))
    , m_local_endpoint_magic(local_endpoint_magic)
{
    m_responsiveness_timer = Core::Timer::create_single_shot(3000, [this] { may_have_become_unresponsive(); });

    m_transport.set_up_read_hook([this] {
        NonnullRefPtr protect = *this;
        // FIXME: Do something about errors.
        (void)drain_messages_from_peer();
        handle_messages();
    });

    m_send_queue = adopt_ref(*new SendQueue);
    m_send_thread = Threading::Thread::construct([this, send_queue = m_send_queue]() -> intptr_t {
        for (;;) {
            send_queue->mutex.lock();
            while (send_queue->messages.is_empty() && send_queue->running)
                send_queue->condition.wait();

            if (!send_queue->running) {
                send_queue->mutex.unlock();
                break;
            }

            auto message_buffer = send_queue->messages.take_first();
            send_queue->mutex.unlock();

            if (auto result = message_buffer.transfer_message(m_transport); result.is_error()) {
                dbgln("ConnectionBase::send_thread: {}", result.error());
                continue;
            }
        }
        return 0;
    });
    m_send_thread->start();
}

ConnectionBase::~ConnectionBase()
{
    {
        Threading::MutexLocker locker(m_send_queue->mutex);
        m_send_queue->running = false;
        m_send_queue->condition.signal();
    }
    (void)m_send_thread->join();
}

bool ConnectionBase::is_open() const
{
    return m_transport.is_open();
}

ErrorOr<void> ConnectionBase::post_message(Message const& message)
{
    return post_message(message.endpoint_magic(), TRY(message.encode()));
}

ErrorOr<void> ConnectionBase::post_message(u32 endpoint_magic, MessageBuffer buffer)
{
    // NOTE: If this connection is being shut down, but has not yet been destroyed,
    //       the socket will be closed. Don't try to send more messages.
    if (!m_transport.is_open())
        return Error::from_string_literal("Trying to post_message during IPC shutdown");

    if (buffer.data().size() > TransportSocket::SOCKET_BUFFER_SIZE) {
        auto wrapper = LargeMessageWrapper::create(endpoint_magic, buffer);
        buffer = MUST(wrapper->encode());
    }

    {
        Threading::MutexLocker locker(m_send_queue->mutex);
        m_send_queue->messages.append(move(buffer));
        m_send_queue->condition.signal();
    }

    m_responsiveness_timer->start();
    return {};
}

void ConnectionBase::shutdown()
{
    m_transport.close();
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
                if (auto post_result = post_message(m_local_endpoint_magic, *response); post_result.is_error()) {
                    dbgln("IPC::ConnectionBase::handle_messages: {}", post_result.error());
                }
            }
        }
    }
}

void ConnectionBase::wait_for_transport_to_become_readable()
{
    m_transport.wait_until_readable();
}

ErrorOr<void> ConnectionBase::drain_messages_from_peer()
{
    auto schedule_shutdown = m_transport.read_as_many_messages_as_possible_without_blocking([&](auto&& unparsed_message) {
        auto const& bytes = unparsed_message.bytes;
        UnprocessedFileDescriptors unprocessed_fds;
        unprocessed_fds.return_fds_to_front_of_queue(move(unparsed_message.fds));
        if (auto message = try_parse_message(bytes, unprocessed_fds)) {
            if (message->message_id() == LargeMessageWrapper::MESSAGE_ID) {
                LargeMessageWrapper* wrapper = static_cast<LargeMessageWrapper*>(message.ptr());
                auto wrapped_message = wrapper->wrapped_message_data();
                unprocessed_fds.return_fds_to_front_of_queue(wrapper->take_fds());
                auto parsed_message = try_parse_message(wrapped_message, unprocessed_fds);
                VERIFY(parsed_message);
                m_unprocessed_messages.append(parsed_message.release_nonnull());
                return;
            }

            m_unprocessed_messages.append(message.release_nonnull());
        } else {
            dbgln("Failed to parse IPC message {:hex-dump}", bytes);
            VERIFY_NOT_REACHED();
        }
    });

    if (!m_unprocessed_messages.is_empty()) {
        m_responsiveness_timer->stop();
        did_become_responsive();
        deferred_invoke([this] {
            handle_messages();
        });
    } else if (schedule_shutdown == TransportSocket::ShouldShutdown::Yes) {
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
