/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibIPC/TransportSocket.h>

namespace IPC {

AutoCloseFileDescriptor::AutoCloseFileDescriptor(int fd)
    : m_fd(fd)
{
}

AutoCloseFileDescriptor::~AutoCloseFileDescriptor()
{
    if (m_fd != -1)
        (void)Core::System::close(m_fd);
}

void SendQueue::enqueue_message(Vector<u8>&& bytes, Vector<int>&& fds)
{
    Threading::MutexLocker locker(m_mutex);
    VERIFY(MUST(m_stream.write_some(bytes.span())) == bytes.size());
    m_fds.append(fds.data(), fds.size());
    m_condition.signal();
}

SendQueue::Running SendQueue::block_until_message_enqueued()
{
    Threading::MutexLocker locker(m_mutex);
    while (m_stream.is_eof() && m_fds.is_empty() && m_running)
        m_condition.wait();
    return m_running ? Running::Yes : Running::No;
}

SendQueue::BytesAndFds SendQueue::peek(size_t max_bytes)
{
    Threading::MutexLocker locker(m_mutex);
    BytesAndFds result;
    auto bytes_to_send = min(max_bytes, m_stream.used_buffer_size());
    result.bytes.resize(bytes_to_send);
    m_stream.peek_some(result.bytes);
    result.fds = m_fds;
    return result;
}

void SendQueue::discard(size_t bytes_count, size_t fds_count)
{
    Threading::MutexLocker locker(m_mutex);
    MUST(m_stream.discard(bytes_count));
    m_fds.remove(0, fds_count);
}

void SendQueue::stop()
{
    Threading::MutexLocker locker(m_mutex);
    m_running = false;
    m_condition.signal();
}

TransportSocket::TransportSocket(NonnullOwnPtr<Core::LocalSocket> socket)
    : m_socket(move(socket))
{
    m_send_queue = adopt_ref(*new SendQueue);
    m_send_thread = Threading::Thread::construct([this, send_queue = m_send_queue]() -> intptr_t {
        for (;;) {
            if (send_queue->block_until_message_enqueued() == SendQueue::Running::No)
                break;

            auto [bytes, fds] = send_queue->peek(4096);
            auto fds_count = fds.size();
            ReadonlyBytes remaining_to_send_bytes = bytes;

            Threading::RWLockLocker<Threading::LockMode::Read> lock(m_socket_rw_lock);
            if (!m_socket->is_open())
                break;
            auto result = send_message(*m_socket, remaining_to_send_bytes, fds);
            if (result.is_error()) {
                if (result.error().is_errno() && result.error().code() == EPIPE) {
                    // The socket is closed from the other end, we can stop sending.
                    break;
                }
                dbgln("TransportSocket::send_thread: {}", result.error());
                VERIFY_NOT_REACHED();
            }

            auto written_bytes_count = bytes.size() - remaining_to_send_bytes.size();
            auto written_fds_count = fds_count - fds.size();
            if (written_bytes_count > 0 || written_fds_count > 0) {
                send_queue->discard(written_bytes_count, written_fds_count);
            }

            if (!m_socket->is_open())
                break;

            {
                Vector<struct pollfd, 1> pollfds;
                pollfds.append({ .fd = m_socket->fd().value(), .events = POLLOUT, .revents = 0 });

                ErrorOr<int> result { 0 };
                do {
                    result = Core::System::poll(pollfds, -1);
                } while (result.is_error() && result.error().code() == EINTR);
            }
        }
        return 0;
    });
    m_send_thread->start();

    (void)Core::System::setsockopt(m_socket->fd().value(), SOL_SOCKET, SO_SNDBUF, &SOCKET_BUFFER_SIZE, sizeof(SOCKET_BUFFER_SIZE));
    (void)Core::System::setsockopt(m_socket->fd().value(), SOL_SOCKET, SO_RCVBUF, &SOCKET_BUFFER_SIZE, sizeof(SOCKET_BUFFER_SIZE));
}

TransportSocket::~TransportSocket()
{
    m_send_queue->stop();
    (void)m_send_thread->join();
}

void TransportSocket::set_up_read_hook(Function<void()> hook)
{
    Threading::RWLockLocker<Threading::LockMode::Write> lock(m_socket_rw_lock);
    VERIFY(m_socket->is_open());
    m_socket->on_ready_to_read = move(hook);
}

bool TransportSocket::is_open() const
{
    Threading::RWLockLocker<Threading::LockMode::Read> lock(m_socket_rw_lock);
    return m_socket->is_open();
}

void TransportSocket::close()
{
    Threading::RWLockLocker<Threading::LockMode::Write> lock(m_socket_rw_lock);
    m_socket->close();
}

void TransportSocket::wait_until_readable()
{
    Threading::RWLockLocker<Threading::LockMode::Read> lock(m_socket_rw_lock);
    auto maybe_did_become_readable = m_socket->can_read_without_blocking(-1);
    if (maybe_did_become_readable.is_error()) {
        dbgln("TransportSocket::wait_until_readable: {}", maybe_did_become_readable.error());
        warnln("TransportSocket::wait_until_readable: {}", maybe_did_become_readable.error());
        VERIFY_NOT_REACHED();
    }

    VERIFY(maybe_did_become_readable.value());
}

struct MessageHeader {
    enum class Type : u8 {
        Payload = 0,
        FileDescriptorAcknowledgement = 1,
    };
    Type type { Type::Payload };
    u32 payload_size { 0 };
    u32 fd_count { 0 };
};

void TransportSocket::post_message(Vector<u8> const& bytes_to_write, Vector<NonnullRefPtr<AutoCloseFileDescriptor>> const& fds)
{
    Vector<u8> message_buffer;
    message_buffer.resize(sizeof(MessageHeader) + bytes_to_write.size());
    MessageHeader header;
    header.payload_size = bytes_to_write.size();
    header.fd_count = fds.size();
    header.type = MessageHeader::Type::Payload;
    memcpy(message_buffer.data(), &header, sizeof(MessageHeader));
    memcpy(message_buffer.data() + sizeof(MessageHeader), bytes_to_write.data(), bytes_to_write.size());

    for (auto const& fd : fds)
        m_fds_retained_until_received_by_peer.enqueue(fd);

    auto raw_fds = Vector<int, 1> {};
    auto num_fds_to_transfer = fds.size();
    if (num_fds_to_transfer > 0) {
        raw_fds.ensure_capacity(num_fds_to_transfer);
        for (auto& owned_fd : fds) {
            raw_fds.unchecked_append(owned_fd->value());
        }
    }

    m_send_queue->enqueue_message(move(message_buffer), move(raw_fds));
}

ErrorOr<void> TransportSocket::send_message(Core::LocalSocket& socket, ReadonlyBytes& bytes_to_write, Vector<int>& unowned_fds)
{
    auto num_fds_to_transfer = unowned_fds.size();
    while (!bytes_to_write.is_empty()) {
        ErrorOr<ssize_t> maybe_nwritten = 0;
        if (num_fds_to_transfer > 0) {
            maybe_nwritten = socket.send_message(bytes_to_write, 0, unowned_fds);
        } else {
            maybe_nwritten = socket.write_some(bytes_to_write);
        }

        if (maybe_nwritten.is_error()) {
            if (auto error = maybe_nwritten.release_error(); error.is_errno() && (error.code() == EAGAIN || error.code() == EWOULDBLOCK || error.code() == EINTR)) {
                return {};
            } else {
                return error;
            }
        }

        bytes_to_write = bytes_to_write.slice(maybe_nwritten.value());
        num_fds_to_transfer = 0;
        unowned_fds.clear();
    }
    return {};
}

TransportSocket::ShouldShutdown TransportSocket::read_as_many_messages_as_possible_without_blocking(Function<void(Message&&)>&& callback)
{
    Threading::RWLockLocker<Threading::LockMode::Read> lock(m_socket_rw_lock);

    bool should_shutdown = false;
    while (is_open()) {
        u8 buffer[4096];
        auto received_fds = Vector<int> {};
        auto maybe_bytes_read = m_socket->receive_message({ buffer, 4096 }, MSG_DONTWAIT, received_fds);
        if (maybe_bytes_read.is_error()) {
            auto error = maybe_bytes_read.release_error();

            if (error.is_errno() && error.code() == EAGAIN) {
                break;
            }
            if (error.is_errno() && error.code() == ECONNRESET) {
                should_shutdown = true;
                break;
            }

            dbgln("TransportSocket::read_as_much_as_possible_without_blocking: {}", error);
            warnln("TransportSocket::read_as_much_as_possible_without_blocking: {}", error);
            VERIFY_NOT_REACHED();
        }

        auto bytes_read = maybe_bytes_read.release_value();
        if (bytes_read.is_empty()) {
            should_shutdown = true;
            break;
        }

        m_unprocessed_bytes.append(bytes_read.data(), bytes_read.size());
        for (auto const& fd : received_fds) {
            m_unprocessed_fds.enqueue(File::adopt_fd(fd));
        }
    }

    u32 received_fd_count = 0;
    u32 acknowledged_fd_count = 0;
    size_t index = 0;
    while (index + sizeof(MessageHeader) <= m_unprocessed_bytes.size()) {
        MessageHeader header;
        memcpy(&header, m_unprocessed_bytes.data() + index, sizeof(MessageHeader));
        if (header.type == MessageHeader::Type::Payload) {
            if (header.payload_size + sizeof(MessageHeader) > m_unprocessed_bytes.size() - index)
                break;
            if (header.fd_count > m_unprocessed_fds.size())
                break;
            Message message;
            received_fd_count += header.fd_count;
            for (size_t i = 0; i < header.fd_count; ++i)
                message.fds.enqueue(m_unprocessed_fds.dequeue());
            message.bytes.append(m_unprocessed_bytes.data() + index + sizeof(MessageHeader), header.payload_size);
            callback(move(message));
        } else if (header.type == MessageHeader::Type::FileDescriptorAcknowledgement) {
            VERIFY(header.payload_size == 0);
            acknowledged_fd_count += header.fd_count;
        } else {
            VERIFY_NOT_REACHED();
        }
        index += header.payload_size + sizeof(MessageHeader);
    }

    if (should_shutdown)
        return ShouldShutdown::Yes;

    if (acknowledged_fd_count > 0) {
        while (acknowledged_fd_count > 0) {
            (void)m_fds_retained_until_received_by_peer.dequeue();
            --acknowledged_fd_count;
        }
    }

    if (received_fd_count > 0) {
        Vector<u8> message_buffer;
        message_buffer.resize(sizeof(MessageHeader));
        MessageHeader header;
        header.payload_size = 0;
        header.fd_count = received_fd_count;
        header.type = MessageHeader::Type::FileDescriptorAcknowledgement;
        memcpy(message_buffer.data(), &header, sizeof(MessageHeader));
        m_send_queue->enqueue_message(move(message_buffer), {});
    }

    if (index < m_unprocessed_bytes.size()) {
        auto remaining_bytes = MUST(ByteBuffer::copy(m_unprocessed_bytes.span().slice(index)));
        m_unprocessed_bytes = move(remaining_bytes);
    } else {
        m_unprocessed_bytes.clear();
    }

    return ShouldShutdown::No;
}

ErrorOr<int> TransportSocket::release_underlying_transport_for_transfer()
{
    Threading::RWLockLocker<Threading::LockMode::Write> lock(m_socket_rw_lock);
    return m_socket->release_fd();
}

ErrorOr<IPC::File> TransportSocket::clone_for_transfer()
{
    Threading::RWLockLocker<Threading::LockMode::Write> lock(m_socket_rw_lock);
    return IPC::File::clone_fd(m_socket->fd().value());
}

}
