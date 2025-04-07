/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibIPC/File.h>
#include <LibIPC/TransportSocket.h>

namespace IPC {

TransportSocket::TransportSocket(NonnullOwnPtr<Core::LocalSocket> socket)
    : m_socket(move(socket))
{
    (void)Core::System::setsockopt(m_socket->fd().value(), SOL_SOCKET, SO_SNDBUF, &SOCKET_BUFFER_SIZE, sizeof(SOCKET_BUFFER_SIZE));
    (void)Core::System::setsockopt(m_socket->fd().value(), SOL_SOCKET, SO_RCVBUF, &SOCKET_BUFFER_SIZE, sizeof(SOCKET_BUFFER_SIZE));
}

TransportSocket::~TransportSocket() = default;

void TransportSocket::set_up_read_hook(Function<void()> hook)
{
    VERIFY(m_socket->is_open());
    m_socket->on_ready_to_read = move(hook);
}

bool TransportSocket::is_open() const
{
    return m_socket->is_open();
}

void TransportSocket::close()
{
    m_socket->close();
}

void TransportSocket::wait_until_readable()
{
    auto maybe_did_become_readable = m_socket->can_read_without_blocking(-1);
    if (maybe_did_become_readable.is_error()) {
        dbgln("TransportSocket::wait_until_readable: {}", maybe_did_become_readable.error());
        warnln("TransportSocket::wait_until_readable: {}", maybe_did_become_readable.error());
        VERIFY_NOT_REACHED();
    }

    VERIFY(maybe_did_become_readable.value());
}

struct MessageHeader {
    u32 size { 0 };
    u32 fd_count { 0 };
};

ErrorOr<void> TransportSocket::transfer_message(ReadonlyBytes bytes_to_write, Vector<int, 1> const& unowned_fds)
{
    Vector<u8> message_buffer;
    message_buffer.resize(sizeof(MessageHeader) + bytes_to_write.size());
    MessageHeader header;
    header.size = bytes_to_write.size();
    header.fd_count = unowned_fds.size();
    memcpy(message_buffer.data(), &header, sizeof(MessageHeader));
    memcpy(message_buffer.data() + sizeof(MessageHeader), bytes_to_write.data(), bytes_to_write.size());
    return transfer(message_buffer.span(), unowned_fds);
}

ErrorOr<void> TransportSocket::transfer(ReadonlyBytes bytes_to_write, Vector<int, 1> const& unowned_fds)
{
    auto num_fds_to_transfer = unowned_fds.size();
    while (!bytes_to_write.is_empty()) {
        ErrorOr<ssize_t> maybe_nwritten = 0;
        if (num_fds_to_transfer > 0) {
            maybe_nwritten = m_socket->send_message(bytes_to_write, 0, unowned_fds);
            if (!maybe_nwritten.is_error())
                num_fds_to_transfer = 0;
        } else {
            maybe_nwritten = m_socket->write_some(bytes_to_write);
        }

        if (maybe_nwritten.is_error()) {
            if (auto error = maybe_nwritten.release_error(); error.is_errno() && (error.code() == EAGAIN || error.code() == EWOULDBLOCK)) {

                // FIXME: Refactor this to pass the unwritten bytes back to the caller to send 'later'
                //        or next time the socket is writable
                Vector<struct pollfd, 1> pollfds;
                if (pollfds.is_empty())
                    pollfds.append({ .fd = m_socket->fd().value(), .events = POLLOUT, .revents = 0 });

                ErrorOr<int> result { 0 };
                do {
                    constexpr u32 POLL_TIMEOUT_MS = 100;
                    result = Core::System::poll(pollfds, POLL_TIMEOUT_MS);
                } while (result.is_error() && result.error().code() == EINTR);

                if (!result.is_error() && result.value() != 0)
                    continue;

                switch (error.code()) {
                case EPIPE:
                    return Error::from_string_literal("IPC::transfer_message: Disconnected from peer");
                case EAGAIN:
                    return Error::from_string_literal("IPC::transfer_message: Timed out waiting for socket to become writable");
                default:
                    return Error::from_syscall("IPC::transfer_message write"sv, -error.code());
                }
            } else {
                return error;
            }
        }

        bytes_to_write = bytes_to_write.slice(maybe_nwritten.value());
    }
    return {};
}

TransportSocket::ShouldShutdown TransportSocket::read_as_many_messages_as_possible_without_blocking(Function<void(Message)>&& callback)
{
    auto should_shutdown = ShouldShutdown::No;
    while (is_open()) {
        u8 buffer[4096];
        auto received_fds = Vector<int> {};
        auto maybe_bytes_read = m_socket->receive_message({ buffer, 4096 }, MSG_DONTWAIT, received_fds);
        if (maybe_bytes_read.is_error()) {
            auto error = maybe_bytes_read.release_error();
            if (error.is_syscall() && error.code() == EAGAIN) {
                break;
            }

            if (error.is_syscall() && error.code() == ECONNRESET) {
                should_shutdown = ShouldShutdown::Yes;
                break;
            }

            dbgln("TransportSocket::read_as_much_as_possible_without_blocking: {}", error);
            warnln("TransportSocket::read_as_much_as_possible_without_blocking: {}", error);
            VERIFY_NOT_REACHED();
        }

        auto bytes_read = maybe_bytes_read.release_value();
        if (bytes_read.is_empty()) {
            should_shutdown = ShouldShutdown::Yes;
            break;
        }

        m_unprocessed_bytes.append(bytes_read.data(), bytes_read.size());
        for (auto const& fd : received_fds) {
            m_unprocessed_fds.enqueue(File::adopt_fd(fd));
        }
    }

    size_t index = 0;
    while (index + sizeof(MessageHeader) < m_unprocessed_bytes.size()) {
        MessageHeader header;
        memcpy(&header, m_unprocessed_bytes.data() + index, sizeof(MessageHeader));
        Message message;
        for (size_t i = 0; i < header.fd_count; ++i)
            message.fds.append(m_unprocessed_fds.dequeue());
        message.bytes.append(m_unprocessed_bytes.data() + index + sizeof(MessageHeader), header.size);
        callback(move(message));
        index += header.size + sizeof(MessageHeader);
    }

    if (index < m_unprocessed_bytes.size()) {
        auto remaining_bytes = MUST(ByteBuffer::copy(m_unprocessed_bytes.span().slice(index)));
        m_unprocessed_bytes = move(remaining_bytes);
    } else {
        m_unprocessed_bytes.clear();
    }

    return should_shutdown;
}

ErrorOr<int> TransportSocket::release_underlying_transport_for_transfer()
{
    return m_socket->release_fd();
}

ErrorOr<IPC::File> TransportSocket::clone_for_transfer()
{
    return IPC::File::clone_fd(m_socket->fd().value());
}

}
