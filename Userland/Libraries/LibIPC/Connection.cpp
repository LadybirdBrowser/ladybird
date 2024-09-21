/*
 * Copyright (c) 2021-2024, Andreas Kling <andreas@ladybird.org>
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

ConnectionBase::ConnectionBase(IPC::Stub& local_stub, NonnullOwnPtr<Core::LocalSocket> socket, u32 local_endpoint_magic)
    : m_local_stub(local_stub)
    , m_socket(move(socket))
    , m_local_endpoint_magic(local_endpoint_magic)
{
    socklen_t socket_buffer_size = 128 * KiB;
    (void)Core::System::setsockopt(m_socket->fd().value(), SOL_SOCKET, SO_SNDBUF, &socket_buffer_size, sizeof(socket_buffer_size));
    (void)Core::System::setsockopt(m_socket->fd().value(), SOL_SOCKET, SO_RCVBUF, &socket_buffer_size, sizeof(socket_buffer_size));

    m_responsiveness_timer = Core::Timer::create_single_shot(3000, [this] { may_have_become_unresponsive(); });
    m_socket->on_ready_to_read = [this] {
        NonnullRefPtr protect = *this;
        // FIXME: Do something about errors.
        (void)drain_messages_from_peer();
        handle_messages();
    };

    m_send_queue = adopt_ref(*new SendQueue);
    m_send_thread = Threading::Thread::construct([this, queue = m_send_queue]() -> intptr_t {
        for (;;) {
            queue->mutex.lock();
            while (queue->messages.is_empty() && queue->running)
                queue->condition.wait();

            if (!queue->running) {
                queue->mutex.unlock();
                break;
            }

            auto message = queue->messages.take_first();
            queue->mutex.unlock();

            if (auto result = message.transfer_message(*m_socket); result.is_error()) {
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
    m_send_thread->detach();
}

bool ConnectionBase::is_open() const
{
    return m_socket->is_open();
}

ErrorOr<void> ConnectionBase::post_message(Message const& message)
{
    return post_message(TRY(message.encode()));
}

ErrorOr<void> ConnectionBase::post_message(MessageBuffer buffer)
{
    // NOTE: If this connection is being shut down, but has not yet been destroyed,
    //       the socket will be closed. Don't try to send more messages.
    if (!m_socket->is_open())
        return Error::from_string_literal("Trying to post_message during IPC shutdown");

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
    m_socket->close();
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
            auto handler_result = m_local_stub.handle(*message);
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

void ConnectionBase::wait_for_socket_to_become_readable()
{
    auto maybe_did_become_readable = m_socket->can_read_without_blocking(-1);
    if (maybe_did_become_readable.is_error()) {
        dbgln("ConnectionBase::wait_for_socket_to_become_readable: {}", maybe_did_become_readable.error());
        warnln("ConnectionBase::wait_for_socket_to_become_readable: {}", maybe_did_become_readable.error());
        VERIFY_NOT_REACHED();
    }

    VERIFY(maybe_did_become_readable.value());
}

ErrorOr<Vector<u8>> ConnectionBase::read_as_much_as_possible_from_socket_without_blocking()
{
    Vector<u8> bytes;

    if (!m_unprocessed_bytes.is_empty()) {
        bytes.append(m_unprocessed_bytes.data(), m_unprocessed_bytes.size());
        m_unprocessed_bytes.clear();
    }

    u8 buffer[4096];
    Vector<int> received_fds;

    bool should_shut_down = false;
    auto schedule_shutdown = [this, &should_shut_down]() {
        should_shut_down = true;
        deferred_invoke([this] {
            shutdown();
        });
    };

    while (m_socket->is_open()) {
        auto maybe_bytes_read = m_socket->receive_message({ buffer, 4096 }, MSG_DONTWAIT, received_fds);
        if (maybe_bytes_read.is_error()) {
            auto error = maybe_bytes_read.release_error();
            if (error.is_syscall() && error.code() == EAGAIN) {
                break;
            }

            if (error.is_syscall() && error.code() == ECONNRESET) {
                schedule_shutdown();
                break;
            }

            dbgln("ConnectionBase::read_as_much_as_possible_from_socket_without_blocking: {}", error);
            warnln("ConnectionBase::read_as_much_as_possible_from_socket_without_blocking: {}", error);
            VERIFY_NOT_REACHED();
        }

        auto bytes_read = maybe_bytes_read.release_value();
        if (bytes_read.is_empty()) {
            schedule_shutdown();
            break;
        }

        bytes.append(bytes_read.data(), bytes_read.size());
        for (auto const& fd : received_fds)
            m_unprocessed_fds.enqueue(IPC::File::adopt_fd(fd));
    }

    if (!bytes.is_empty()) {
        m_responsiveness_timer->stop();
        did_become_responsive();
    } else if (should_shut_down) {
        return Error::from_string_literal("IPC connection EOF");
    }

    return bytes;
}

ErrorOr<void> ConnectionBase::drain_messages_from_peer()
{
    auto bytes = TRY(read_as_much_as_possible_from_socket_without_blocking());

    size_t index = 0;
    try_parse_messages(bytes, index);

    if (index < bytes.size()) {
        // Sometimes we might receive a partial message. That's okay, just stash away
        // the unprocessed bytes and we'll prepend them to the next incoming message
        // in the next run of this function.
        auto remaining_bytes = TRY(ByteBuffer::copy(bytes.span().slice(index)));
        if (!m_unprocessed_bytes.is_empty()) {
            shutdown();
            return Error::from_string_literal("drain_messages_from_peer: Already have unprocessed bytes");
        }
        m_unprocessed_bytes = move(remaining_bytes);
    }

    if (!m_unprocessed_messages.is_empty()) {
        deferred_invoke([this] {
            handle_messages();
        });
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

        if (!m_socket->is_open())
            break;

        wait_for_socket_to_become_readable();
        if (drain_messages_from_peer().is_error())
            break;
    }
    return {};
}

void ConnectionBase::try_parse_messages(Vector<u8> const& bytes, size_t& index)
{
    u32 message_size = 0;
    for (; index + sizeof(message_size) < bytes.size(); index += message_size) {
        memcpy(&message_size, bytes.data() + index, sizeof(message_size));
        if (message_size == 0 || bytes.size() - index - sizeof(uint32_t) < message_size)
            break;
        index += sizeof(message_size);
        auto remaining_bytes = ReadonlyBytes { bytes.data() + index, message_size };

        if (auto message = try_parse_message(remaining_bytes, m_unprocessed_fds)) {
            m_unprocessed_messages.append(message.release_nonnull());
            continue;
        }

        dbgln("Failed to parse IPC message:");
        dbgln("{:hex-dump}", remaining_bytes);
        break;
    }
}

}
