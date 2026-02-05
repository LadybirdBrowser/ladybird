/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Types.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibIPC/Limits.h>
#include <LibIPC/TransportSocket.h>
#include <LibThreading/Thread.h>

namespace IPC {

void SendQueue::enqueue_message(Vector<u8>&& bytes, Vector<int>&& fds)
{
    Threading::MutexLocker locker(m_mutex);
    VERIFY(MUST(m_stream.write_some(bytes.span())) == bytes.size());
    m_fds.append(fds.data(), fds.size());
}

SendQueue::BytesAndFds SendQueue::peek(size_t max_bytes)
{
    Threading::MutexLocker locker(m_mutex);
    BytesAndFds result;
    auto bytes_to_send = min(max_bytes, m_stream.used_buffer_size());
    result.bytes.resize(bytes_to_send);
    m_stream.peek_some(result.bytes);

    if (m_fds.size() > 0) {
        auto fds_to_send = min(m_fds.size(), Core::LocalSocket::MAX_TRANSFER_FDS);
        result.fds = Vector<int> { m_fds.span().slice(0, fds_to_send) };
        // NOTE: This relies on a subsequent call to discard to actually remove the fds from m_fds
    }
    return result;
}

void SendQueue::discard(size_t bytes_count, size_t fds_count)
{
    Threading::MutexLocker locker(m_mutex);
    MUST(m_stream.discard(bytes_count));
    m_fds.remove(0, fds_count);
}

TransportSocket::TransportSocket(NonnullOwnPtr<Core::LocalSocket> socket)
    : m_socket(move(socket))
{
    (void)Core::System::setsockopt(m_socket->fd().value(), SOL_SOCKET, SO_SNDBUF, &SOCKET_BUFFER_SIZE, sizeof(SOCKET_BUFFER_SIZE));
    (void)Core::System::setsockopt(m_socket->fd().value(), SOL_SOCKET, SO_RCVBUF, &SOCKET_BUFFER_SIZE, sizeof(SOCKET_BUFFER_SIZE));

    m_send_queue = adopt_ref(*new SendQueue);

    {
        auto fds = MUST(Core::System::pipe2(O_CLOEXEC | O_NONBLOCK));
        m_wakeup_io_thread_read_fd = adopt_ref(*new AutoCloseFileDescriptor(fds[0]));
        m_wakeup_io_thread_write_fd = adopt_ref(*new AutoCloseFileDescriptor(fds[1]));
    }

    {
        auto fds = MUST(Core::System::pipe2(O_CLOEXEC | O_NONBLOCK));
        m_notify_hook_read_fd = adopt_ref(*new AutoCloseFileDescriptor(fds[0]));
        m_notify_hook_write_fd = adopt_ref(*new AutoCloseFileDescriptor(fds[1]));
    }

    m_io_thread = Threading::Thread::construct("IPC IO"sv, [this] { return io_thread_loop(); });
    m_io_thread->start();
}

intptr_t TransportSocket::io_thread_loop()
{
    Array<struct pollfd, 2> pollfds;
    for (;;) {
        auto want_to_write = [&] {
            auto [bytes, fds] = m_send_queue->peek(1);
            return !bytes.is_empty() || !fds.is_empty();
        }();

        auto state = m_io_thread_state.load();
        if (state == IOThreadState::Stopped)
            break;
        if (state == IOThreadState::SendPendingMessagesAndStop && !want_to_write) {
            m_io_thread_state = IOThreadState::Stopped;
            break;
        }

        short events = POLLIN;
        if (want_to_write)
            events |= POLLOUT;
        pollfds[0] = { .fd = m_socket->fd().value(), .events = events, .revents = 0 };
        pollfds[1] = { .fd = m_wakeup_io_thread_read_fd->value(), .events = POLLIN, .revents = 0 };

        ErrorOr<int> result { 0 };
        do {
            result = Core::System::poll(pollfds, -1);
        } while (result.is_error() && result.error().code() == EINTR);
        if (result.is_error()) {
            dbgln("TransportSocket poll error: {}", result.error());
            m_io_thread_state = IOThreadState::Stopped;
            break;
        }

        if (pollfds[1].revents & POLLIN) {
            char buf[64];
            // The wakeup pipe is non-blocking, so EAGAIN is possible if there's a spurious wakeup.
            (void)Core::System::read(m_wakeup_io_thread_read_fd->value(), { buf, sizeof(buf) });
        }

        if (pollfds[0].revents & POLLIN)
            read_incoming_messages();

        if (pollfds[0].revents & POLLHUP) {
            m_io_thread_state = IOThreadState::Stopped;
            break;
        }

        if (pollfds[0].revents & (POLLERR | POLLNVAL)) {
            dbgln("TransportSocket poll: socket error (POLLERR or POLLNVAL)");
            m_io_thread_state = IOThreadState::Stopped;
            break;
        }

        if (pollfds[0].revents & POLLOUT) {
            auto [bytes, fds] = m_send_queue->peek(4096);
            if (!bytes.is_empty() || !fds.is_empty()) {
                ReadonlyBytes remaining = bytes;
                if (transfer_data(remaining, fds) == TransferState::SocketClosed) {
                    m_io_thread_state = IOThreadState::Stopped;
                }
            }
        }
    }

    VERIFY(m_io_thread_state == IOThreadState::Stopped);
    m_peer_eof = true;
    m_incoming_cv.broadcast();
    return 0;
}

void TransportSocket::wake_io_thread()
{
    Array<u8, 1> bytes = { 0 };
    (void)Core::System::write(m_wakeup_io_thread_write_fd->value(), bytes);
}

TransportSocket::~TransportSocket()
{
    stop_io_thread(IOThreadState::Stopped);
    m_read_hook_notifier.clear();
}

void TransportSocket::stop_io_thread(IOThreadState desired_state)
{
    VERIFY(desired_state == IOThreadState::Stopped || desired_state == IOThreadState::SendPendingMessagesAndStop);
    m_io_thread_state.store(desired_state, AK::MemoryOrder::memory_order_release);
    wake_io_thread();
    if (m_io_thread && m_io_thread->needs_to_be_joined())
        (void)m_io_thread->join();
}

void TransportSocket::set_up_read_hook(Function<void()> hook)
{
    m_on_read_hook = move(hook);
    m_read_hook_notifier = Core::Notifier::construct(m_notify_hook_read_fd->value(), Core::NotificationType::Read);
    m_read_hook_notifier->on_activation = [this] {
        VERIFY(m_notify_hook_read_fd);
        char buf[64];
        (void)Core::System::read(m_notify_hook_read_fd->value(), { buf, sizeof(buf) });
        if (m_on_read_hook)
            m_on_read_hook();
    };

    {
        Threading::MutexLocker locker(m_incoming_mutex);
        if (!m_incoming_messages.is_empty()) {
            Array<u8, 1> bytes = { 0 };
            MUST(Core::System::write(m_notify_hook_write_fd->value(), bytes));
        }
    }
}

bool TransportSocket::is_open() const
{
    return m_socket->is_open();
}

void TransportSocket::close()
{
    stop_io_thread(IOThreadState::Stopped);
    m_socket->close();
}

void TransportSocket::close_after_sending_all_pending_messages()
{
    stop_io_thread(IOThreadState::SendPendingMessagesAndStop);
    m_socket->close();
}

void TransportSocket::wait_until_readable()
{
    Threading::MutexLocker lock(m_incoming_mutex);
    while (m_incoming_messages.is_empty() && m_io_thread_state == IOThreadState::Running) {
        m_incoming_cv.wait();
    }
}

// Maximum size of accumulated unprocessed bytes before we disconnect the peer
static constexpr size_t MAX_UNPROCESSED_BUFFER_SIZE = 128 * MiB;

// Maximum number of accumulated unprocessed file descriptors before we disconnect the peer
static constexpr size_t MAX_UNPROCESSED_FDS = 512;

struct MessageHeader {
    enum class Type : u8 {
        Payload = 0,
        FileDescriptorAcknowledgement = 1,
    };
    Type type { Type::Payload };
    u32 payload_size { 0 };
    u32 fd_count { 0 };

    static Vector<u8> encode_with_payload(MessageHeader header, ReadonlyBytes payload)
    {
        Vector<u8> message_buffer;
        message_buffer.resize(sizeof(MessageHeader) + payload.size());
        memcpy(message_buffer.data(), &header, sizeof(MessageHeader));
        memcpy(message_buffer.data() + sizeof(MessageHeader), payload.data(), payload.size());
        return message_buffer;
    }
};

void TransportSocket::post_message(Vector<u8> const& bytes_to_write, Vector<NonnullRefPtr<AutoCloseFileDescriptor>> const& fds)
{
    auto num_fds_to_transfer = fds.size();

    auto message_buffer = MessageHeader::encode_with_payload(
        {
            .type = MessageHeader::Type::Payload,
            .payload_size = static_cast<u32>(bytes_to_write.size()),
            .fd_count = static_cast<u32>(num_fds_to_transfer),
        },
        bytes_to_write);

    {
        Threading::MutexLocker locker(m_fds_retained_until_received_by_peer_mutex);
        for (auto const& fd : fds)
            m_fds_retained_until_received_by_peer.enqueue(fd);
    }

    auto raw_fds = Vector<int, 1> {};
    if (num_fds_to_transfer > 0) {
        raw_fds.ensure_capacity(num_fds_to_transfer);
        for (auto const& owned_fd : fds) {
            raw_fds.unchecked_append(owned_fd->value());
        }
    }

    m_send_queue->enqueue_message(move(message_buffer), move(raw_fds));
    wake_io_thread();
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

TransportSocket::TransferState TransportSocket::transfer_data(ReadonlyBytes& bytes, Vector<int>& fds)
{
    auto byte_count = bytes.size();
    auto fd_count = fds.size();

    if (auto result = send_message(*m_socket, bytes, fds); result.is_error()) {
        if (result.error().is_errno() && result.error().code() == EPIPE) {
            // The socket is closed from the other end, we can stop sending.
            return TransferState::SocketClosed;
        }

        dbgln("TransportSocket::send_thread: {}", result.error());
        return TransferState::SocketClosed;
    }

    auto written_byte_count = byte_count - bytes.size();
    auto written_fd_count = fd_count - fds.size();
    if (written_byte_count > 0 || written_fd_count > 0)
        m_send_queue->discard(written_byte_count, written_fd_count);

    return TransferState::Continue;
}

void TransportSocket::read_incoming_messages()
{
    Vector<NonnullOwnPtr<Message>> batch;
    while (m_socket->is_open()) {
        u8 buffer[4096];
        auto received_fds = Vector<int> {};
        auto maybe_bytes_read = m_socket->receive_message({ buffer, 4096 }, MSG_DONTWAIT, received_fds);
        if (maybe_bytes_read.is_error()) {
            auto error = maybe_bytes_read.release_error();

            if (error.is_errno() && error.code() == EAGAIN) {
                break;
            }
            if (error.is_errno() && error.code() == ECONNRESET) {
                m_peer_eof = true;
                break;
            }

            dbgln("TransportSocket::read_as_much_as_possible_without_blocking: {}", error);
            warnln("TransportSocket::read_as_much_as_possible_without_blocking: {}", error);
            m_peer_eof = true;
            break;
        }

        auto bytes_read = maybe_bytes_read.release_value();
        if (bytes_read.is_empty() && received_fds.is_empty()) {
            m_peer_eof = true;
            break;
        }

        if (m_unprocessed_bytes.size() + bytes_read.size() > MAX_UNPROCESSED_BUFFER_SIZE) {
            dbgln("TransportSocket: Unprocessed buffer would exceed {} bytes, disconnecting peer", MAX_UNPROCESSED_BUFFER_SIZE);
            m_peer_eof = true;
            break;
        }
        if (m_unprocessed_bytes.try_append(bytes_read.data(), bytes_read.size()).is_error()) {
            dbgln("TransportSocket: Failed to append to unprocessed_bytes buffer");
            m_peer_eof = true;
            break;
        }
        if (m_unprocessed_fds.size() + received_fds.size() > MAX_UNPROCESSED_FDS) {
            dbgln("TransportSocket: Unprocessed FDs would exceed {}, disconnecting peer", MAX_UNPROCESSED_FDS);
            m_peer_eof = true;
            break;
        }
        for (auto const& fd : received_fds) {
            m_unprocessed_fds.enqueue(File::adopt_fd(fd));
        }
    }

    Checked<u32> received_fd_count = 0;
    Checked<u32> acknowledged_fd_count = 0;
    size_t index = 0;
    while (index + sizeof(MessageHeader) <= m_unprocessed_bytes.size()) {
        MessageHeader header;
        memcpy(&header, m_unprocessed_bytes.data() + index, sizeof(MessageHeader));
        if (header.type == MessageHeader::Type::Payload) {
            if (header.payload_size > MAX_MESSAGE_PAYLOAD_SIZE) {
                dbgln("TransportSocket: Rejecting message with payload_size {} exceeding limit {}", header.payload_size, MAX_MESSAGE_PAYLOAD_SIZE);
                m_peer_eof = true;
                break;
            }
            if (header.fd_count > MAX_MESSAGE_FD_COUNT) {
                dbgln("TransportSocket: Rejecting message with fd_count {} exceeding limit {}", header.fd_count, MAX_MESSAGE_FD_COUNT);
                m_peer_eof = true;
                break;
            }
            Checked<size_t> message_size = header.payload_size;
            message_size += sizeof(MessageHeader);
            if (message_size.has_overflow() || message_size.value() > m_unprocessed_bytes.size() - index)
                break;
            if (header.fd_count > m_unprocessed_fds.size())
                break;
            auto message = make<Message>();
            received_fd_count += header.fd_count;
            if (received_fd_count.has_overflow()) {
                dbgln("TransportSocket: received_fd_count would overflow");
                m_peer_eof = true;
                break;
            }
            for (size_t i = 0; i < header.fd_count; ++i)
                message->fds.enqueue(m_unprocessed_fds.dequeue());
            if (message->bytes.try_append(m_unprocessed_bytes.data() + index + sizeof(MessageHeader), header.payload_size).is_error()) {
                dbgln("TransportSocket: Failed to allocate message buffer for payload_size {}", header.payload_size);
                m_peer_eof = true;
                break;
            }
            batch.append(move(message));
        } else if (header.type == MessageHeader::Type::FileDescriptorAcknowledgement) {
            if (header.payload_size != 0) {
                dbgln("TransportSocket: FileDescriptorAcknowledgement with non-zero payload_size {}", header.payload_size);
                m_peer_eof = true;
                break;
            }
            acknowledged_fd_count += header.fd_count;
            if (acknowledged_fd_count.has_overflow()) {
                dbgln("TransportSocket: acknowledged_fd_count would overflow");
                m_peer_eof = true;
                break;
            }
        } else {
            dbgln("TransportSocket: Unknown message header type {}", static_cast<u8>(header.type));
            m_peer_eof = true;
            break;
        }
        Checked<size_t> new_index = index;
        new_index += header.payload_size;
        new_index += sizeof(MessageHeader);
        if (new_index.has_overflow()) {
            dbgln("TransportSocket: index would overflow");
            m_peer_eof = true;
            break;
        }
        index = new_index.value();
    }

    if (acknowledged_fd_count > 0u) {
        Threading::MutexLocker locker(m_fds_retained_until_received_by_peer_mutex);
        while (acknowledged_fd_count > 0u) {
            if (m_fds_retained_until_received_by_peer.is_empty()) {
                dbgln("TransportSocket: Peer acknowledged more FDs than we sent");
                m_peer_eof = true;
                break;
            }
            (void)m_fds_retained_until_received_by_peer.dequeue();
            --acknowledged_fd_count;
        }
    }

    if (received_fd_count > 0u) {
        Vector<u8> message_buffer;
        message_buffer.resize(sizeof(MessageHeader));
        MessageHeader header;
        header.payload_size = 0;
        header.fd_count = received_fd_count.value();
        header.type = MessageHeader::Type::FileDescriptorAcknowledgement;
        memcpy(message_buffer.data(), &header, sizeof(MessageHeader));
        m_send_queue->enqueue_message(move(message_buffer), {});
        wake_io_thread();
    }

    if (index < m_unprocessed_bytes.size()) {
        auto remaining_bytes_or_error = ByteBuffer::copy(m_unprocessed_bytes.span().slice(index));
        if (remaining_bytes_or_error.is_error()) {
            dbgln("TransportSocket: Failed to copy remaining bytes");
            m_peer_eof = true;
        } else {
            m_unprocessed_bytes = remaining_bytes_or_error.release_value();
        }
    } else {
        m_unprocessed_bytes.clear();
    }

    auto notify_read_available = [&] {
        Array<u8, 1> bytes = { 0 };
        (void)Core::System::write(m_notify_hook_write_fd->value(), bytes);
    };

    if (!batch.is_empty()) {
        Threading::MutexLocker locker(m_incoming_mutex);
        m_incoming_messages.extend(move(batch));
        m_incoming_cv.broadcast();
        notify_read_available();
    }

    if (m_peer_eof) {
        m_incoming_cv.broadcast();
        notify_read_available();
    }
}

TransportSocket::ShouldShutdown TransportSocket::read_as_many_messages_as_possible_without_blocking(Function<void(Message&&)>&& callback)
{
    Vector<NonnullOwnPtr<Message>> messages;
    {
        Threading::MutexLocker locker(m_incoming_mutex);
        messages = move(m_incoming_messages);
    }
    for (auto& message : messages)
        callback(move(*message));
    return m_peer_eof ? ShouldShutdown::Yes : ShouldShutdown::No;
}

ErrorOr<int> TransportSocket::release_underlying_transport_for_transfer()
{
    stop_io_thread(IOThreadState::SendPendingMessagesAndStop);
    return m_socket->release_fd();
}

ErrorOr<IPC::File> TransportSocket::clone_for_transfer()
{
    return IPC::File::clone_fd(m_socket->fd().value());
}

}
