/*
 * Copyright (c) 2021, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/System.h>
#include <LibIPC/Connection.h>
#include <LibIPC/File.h>
#include <LibIPC/Stub.h>

namespace IPC {

ConnectionBase::ConnectionBase(IPC::Stub& local_stub, NonnullOwnPtr<Core::LocalSocket> socket, u32 local_endpoint_magic)
    : m_local_stub(local_stub)
    , m_socket(move(socket))
    , m_local_endpoint_magic(local_endpoint_magic)
{
    m_responsiveness_timer = Core::Timer::create_single_shot(3000, [this] { may_have_become_unresponsive(); });
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

    if (auto result = buffer.transfer_message(*m_socket); result.is_error()) {
        shutdown_with_error(result.error());
        return result.release_error();
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

}
