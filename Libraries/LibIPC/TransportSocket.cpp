/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/ScopeGuard.h>
#include <AK/Types.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/File.h>
#include <LibIPC/Limits.h>
#include <LibIPC/TransportHandle.h>
#include <LibIPC/TransportSocket.h>
#include <LibSync/Mutex.h>
#include <LibThreading/Thread.h>

namespace IPC {

Atomic<u32> TransportSocket::s_eof_drain_window_for_test_ms { 0 };
Atomic<bool> TransportSocket::s_skip_inloop_read_for_test { false };

void TransportSocket::set_eof_drain_window_for_test(u32 milliseconds)
{
    s_eof_drain_window_for_test_ms.store(milliseconds, AK::MemoryOrder::memory_order_relaxed);
}

void TransportSocket::set_skip_inloop_read_for_test(bool skip)
{
    s_skip_inloop_read_for_test.store(skip, AK::MemoryOrder::memory_order_relaxed);
}

ErrorOr<NonnullOwnPtr<TransportSocket>> TransportSocket::from_socket(NonnullOwnPtr<Core::LocalSocket> socket)
{
    return make<TransportSocket>(move(socket));
}

ErrorOr<TransportSocket::Paired> TransportSocket::create_paired()
{
    int fds[2] {};
    TRY(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, fds));

    ArmedScopeGuard guard_fd_0 { [&] { MUST(Core::System::close(fds[0])); } };
    ArmedScopeGuard guard_fd_1 { [&] { MUST(Core::System::close(fds[1])); } };

    auto socket0 = TRY(Core::LocalSocket::adopt_fd(fds[0]));
    guard_fd_0.disarm();
    TRY(socket0->set_close_on_exec(true));
    TRY(socket0->set_blocking(false));

    TRY(Core::System::set_close_on_exec(fds[1], true));
    guard_fd_1.disarm();

    // Local side gets a full transport; remote side is just a handle containing the raw fd for transfer to another process.
    return Paired {
        make<TransportSocket>(move(socket0)),
        TransportHandle { File::adopt_fd(fds[1]) },
    };
}

void SendQueue::enqueue_message(SocketMessageHeader header, MessageDataType payload, Vector<int>&& fds)
{
    Sync::MutexLocker locker(m_mutex);
    m_queued_byte_count += sizeof(SocketMessageHeader) + payload.size();
    m_queued_messages.append(QueuedMessage { header, move(payload) });
    m_fds.append(fds.data(), fds.size());
}

SendQueue::BytesAndFds SendQueue::peek(size_t max_bytes)
{
    Sync::MutexLocker locker(m_mutex);
    BytesAndFds result;
    auto bytes_to_send = min(max_bytes, m_queued_byte_count);
    result.bytes.resize(bytes_to_send);
    size_t copied_bytes = 0;
    for (auto const& queued_message : m_queued_messages) {
        if (copied_bytes == bytes_to_send)
            break;

        auto start_offset = queued_message.start_offset;
        if (start_offset < sizeof(SocketMessageHeader)) {
            ReadonlyBytes header { reinterpret_cast<u8 const*>(&queued_message.header), sizeof(SocketMessageHeader) };
            copied_bytes += header.slice(start_offset).copy_trimmed_to(result.bytes.span().slice(copied_bytes));
            if (copied_bytes == bytes_to_send)
                break;
            start_offset = sizeof(SocketMessageHeader);
        }

        auto payload_offset = start_offset - sizeof(SocketMessageHeader);
        if (payload_offset < queued_message.payload.size()) {
            ReadonlyBytes payload { queued_message.payload.data() + payload_offset, queued_message.payload.size() - payload_offset };
            copied_bytes += payload.copy_trimmed_to(result.bytes.span().slice(copied_bytes));
        }
    }

    if (m_fds.size() > 0) {
        auto fds_to_send = min(m_fds.size(), Core::LocalSocket::MAX_TRANSFER_FDS);
        result.fds = Vector<int> { m_fds.span().slice(0, fds_to_send) };
        // NOTE: This relies on a subsequent call to discard to actually remove the fds from m_fds
    }
    return result;
}

void SendQueue::discard(size_t bytes_count, size_t fds_count)
{
    Sync::MutexLocker locker(m_mutex);
    VERIFY(bytes_count <= m_queued_byte_count);
    m_queued_byte_count -= bytes_count;
    while (bytes_count > 0) {
        auto& queued_message = m_queued_messages.first();
        auto available_bytes = queued_message.size() - queued_message.start_offset;
        auto consumed_bytes = min(available_bytes, bytes_count);
        queued_message.start_offset += consumed_bytes;
        bytes_count -= consumed_bytes;
        if (queued_message.start_offset == queued_message.size())
            (void)m_queued_messages.remove(m_queued_messages.begin());
    }
    m_fds.remove(0, fds_count);
}

TransportSocket::TransportSocket(NonnullOwnPtr<Core::LocalSocket> socket)
    : m_socket(move(socket))
{
    // Disable the socket's built-in notifier. TransportSocket uses its own pipe-based notification mechanism on the IO
    // thread, so this notifier is unused. Otherwise, when the socket reaches EOF, this notifier is disabled from the IO
    // thread. In the Qt UI, this causes QSocketNotifier destruction to be deferred. If the socket is closed before the
    // deferred destruction runs, Qt detects an invalid socket and prints a warning.
    m_socket->set_notifications_enabled(false);

    (void)Core::System::setsockopt(m_socket->fd().value(), SOL_SOCKET, SO_SNDBUF, &SOCKET_BUFFER_SIZE, sizeof(SOCKET_BUFFER_SIZE));
    (void)Core::System::setsockopt(m_socket->fd().value(), SOL_SOCKET, SO_RCVBUF, &SOCKET_BUFFER_SIZE, sizeof(SOCKET_BUFFER_SIZE));

    m_send_queue = adopt_ref(*new SendQueue);

    auto fds = MUST(Core::System::pipe2(O_CLOEXEC | O_NONBLOCK));
    m_wakeup_io_thread_read_fd = adopt_ref(*new AutoCloseFileDescriptor(fds[0]));
    m_wakeup_io_thread_write_fd = adopt_ref(*new AutoCloseFileDescriptor(fds[1]));

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

        if ((pollfds[0].revents & POLLIN) && !s_skip_inloop_read_for_test.load(AK::MemoryOrder::memory_order_relaxed))
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
    if (!m_is_being_transferred.load(AK::MemoryOrder::memory_order_acquire)) {
        // The loop may have stopped on a send-side failure (the peer closed its end while we still had data queued to
        // send — so transfer_data() returned SocketClosed) without reading a final inbound message left buffered on the
        // socket. Drain it before publishing EOF — so a message that arrived just before teardown (e.g., a cross-realm
        // transform stream's "error") is actually delivered — rather than discarded.
        read_incoming_messages();

        Sync::MutexLocker locker(m_incoming_mutex);
        m_peer_eof = true;
        m_incoming_eof = true;
        m_incoming_cv.broadcast();
        notify_read_available();
    }
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

void TransportSocket::notify_read_available()
{
    if (!m_notify_hook_write_fd)
        return;
    Array<u8, 1> bytes = { 0 };
    (void)Core::System::write(m_notify_hook_write_fd->value(), bytes);
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
        Sync::MutexLocker locker(m_incoming_mutex);
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
    Sync::MutexLocker lock(m_incoming_mutex);
    while (m_incoming_messages.is_empty() && m_io_thread_state == IOThreadState::Running) {
        m_incoming_cv.wait();
    }
}

// Maximum size of accumulated unprocessed bytes before we disconnect the peer
static constexpr size_t MAX_UNPROCESSED_BUFFER_SIZE = 128 * MiB;

// Maximum number of accumulated unprocessed file descriptors before we disconnect the peer
static constexpr size_t MAX_UNPROCESSED_FDS = 512;

void TransportSocket::post_message(MessageDataType bytes_to_write, Vector<Attachment>& attachments)
{
    auto num_fds_to_transfer = attachments.size();

    SocketMessageHeader header {
        .type = SocketMessageHeader::Type::Payload,
        .payload_size = static_cast<u32>(bytes_to_write.size()),
        .fd_count = static_cast<u32>(num_fds_to_transfer),
    };

    auto raw_fds = Vector<int, 1> {};
    if (num_fds_to_transfer > 0) {
        raw_fds.ensure_capacity(num_fds_to_transfer);
        Sync::MutexLocker locker(m_fds_retained_until_received_by_peer_mutex);
        for (auto& attachment : attachments) {
            int fd = attachment.to_fd();
            auto auto_fd = adopt_ref(*new AutoCloseFileDescriptor(fd));
            raw_fds.unchecked_append(auto_fd->value());
            m_fds_retained_until_received_by_peer.enqueue(move(auto_fd));
        }
    }

    m_send_queue->enqueue_message(header, move(bytes_to_write), move(raw_fds));
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
        if (m_unprocessed_attachments.size() + received_fds.size() > MAX_UNPROCESSED_FDS) {
            dbgln("TransportSocket: Unprocessed FDs would exceed {}, disconnecting peer", MAX_UNPROCESSED_FDS);
            m_peer_eof = true;
            break;
        }
        for (auto const& fd : received_fds) {
            m_unprocessed_attachments.enqueue(Attachment::from_fd(fd));
        }
    }

    if (m_peer_eof) {
        if (auto window_ms = s_eof_drain_window_for_test_ms.load(AK::MemoryOrder::memory_order_relaxed); window_ms != 0) {
            notify_read_available();
            (void)Core::System::sleep_ms(window_ms);
        }
    }

    Checked<u32> received_fd_count = 0;
    Checked<u32> acknowledged_fd_count = 0;
    size_t index = 0;
    while (index + sizeof(SocketMessageHeader) <= m_unprocessed_bytes.size()) {
        SocketMessageHeader header;
        memcpy(&header, m_unprocessed_bytes.data() + index, sizeof(SocketMessageHeader));
        if (header.type == SocketMessageHeader::Type::Payload) {
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
            message_size += sizeof(SocketMessageHeader);
            if (message_size.has_overflow() || message_size.value() > m_unprocessed_bytes.size() - index)
                break;
            if (header.fd_count > m_unprocessed_attachments.size())
                break;
            auto message = make<Message>();
            received_fd_count += header.fd_count;
            if (received_fd_count.has_overflow()) {
                dbgln("TransportSocket: received_fd_count would overflow");
                m_peer_eof = true;
                break;
            }
            for (size_t i = 0; i < header.fd_count; ++i)
                message->attachments.enqueue(m_unprocessed_attachments.dequeue());
            Vector<u8> payload_bytes;
            if (payload_bytes.try_append(m_unprocessed_bytes.data() + index + sizeof(SocketMessageHeader), header.payload_size).is_error()) {
                dbgln("TransportSocket: Failed to allocate message buffer for payload_size {}", header.payload_size);
                m_peer_eof = true;
                break;
            }
            message->bytes = ReceivedMessageBytes::from_vector(move(payload_bytes));
            batch.append(move(message));
        } else if (header.type == SocketMessageHeader::Type::FileDescriptorAcknowledgement) {
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
        new_index += sizeof(SocketMessageHeader);
        if (new_index.has_overflow()) {
            dbgln("TransportSocket: index would overflow");
            m_peer_eof = true;
            break;
        }
        index = new_index.value();
    }

    if (acknowledged_fd_count > 0u) {
        Sync::MutexLocker locker(m_fds_retained_until_received_by_peer_mutex);
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
        SocketMessageHeader header {
            .type = SocketMessageHeader::Type::FileDescriptorAcknowledgement,
            .payload_size = 0,
            .fd_count = received_fd_count.value(),
        };
        m_send_queue->enqueue_message(header, {}, {});
        wake_io_thread();
    }

    if (index < m_unprocessed_bytes.size()) {
        auto remaining = m_unprocessed_bytes.size() - index;
        m_unprocessed_bytes.overwrite(0, m_unprocessed_bytes.data() + index, remaining);
        m_unprocessed_bytes.resize(remaining);
    } else {
        m_unprocessed_bytes.clear();
    }

    bool const peer_eof = m_peer_eof;
    if (!batch.is_empty() || peer_eof) {
        Sync::MutexLocker locker(m_incoming_mutex);
        if (!batch.is_empty())
            m_incoming_messages.extend(move(batch));
        // Publish EOF only after the final batch is appended — under the same lock the consumer drains with — so that a
        // consumer which observes EOF has also taken every message that arrived before it.
        if (peer_eof)
            m_incoming_eof = true;
        m_incoming_cv.broadcast();
        notify_read_available();
    }
}

TransportSocket::ShouldShutdown TransportSocket::read_as_many_messages_as_possible_without_blocking(Function<void(Message&&)>&& callback)
{
    Vector<NonnullOwnPtr<Message>> messages;
    bool eof;
    {
        Sync::MutexLocker locker(m_incoming_mutex);
        messages = move(m_incoming_messages);
        eof = m_incoming_eof;
    }
    for (auto& message : messages)
        callback(move(*message));
    return eof ? ShouldShutdown::Yes : ShouldShutdown::No;
}

ErrorOr<TransportHandle> TransportSocket::release_for_transfer()
{
    m_is_being_transferred.store(true, AK::MemoryOrder::memory_order_release);
    stop_io_thread(IOThreadState::SendPendingMessagesAndStop);
    auto fd = TRY(m_socket->release_fd());
    return TransportHandle { File::adopt_fd(fd) };
}

}
