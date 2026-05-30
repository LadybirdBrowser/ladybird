/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteReader.h>
#include <AK/Checked.h>
#include <AK/Debug.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/ScopeGuard.h>
#include <AK/Types.h>
#include <AK/Windows.h>
#include <LibCore/Notifier.h>
#include <LibCore/System.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/File.h>
#include <LibIPC/HandleType.h>
#include <LibIPC/Limits.h>
#include <LibIPC/TransportHandle.h>
#include <LibIPC/TransportSocketWindows.h>
#include <LibThreading/Thread.h>

#include <AK/Windows.h>

#ifndef IPC_DEBUG
#    define IPC_DEBUG 0
#endif

namespace IPC {

static constexpr size_t MAX_SERIALIZED_ATTACHMENT_SIZE = sizeof(HandleType) + sizeof(WSAPROTOCOL_INFOW);
static constexpr size_t MAX_ATTACHMENT_DATA_SIZE = MAX_MESSAGE_FD_COUNT * MAX_SERIALIZED_ATTACHMENT_SIZE;
static constexpr socklen_t SOCKET_BUFFER_SIZE = 128 * KiB;

void SendQueue::enqueue_message(ReadonlyBytes header, ReadonlyBytes attachments, ReadonlyBytes payload)
{
    Sync::MutexLocker locker(m_mutex);
    VERIFY(MUST(m_stream.write_some(header)) == header.size());
    if (!attachments.is_empty())
        VERIFY(MUST(m_stream.write_some(attachments)) == attachments.size());
    if (!payload.is_empty())
        VERIFY(MUST(m_stream.write_some(payload)) == payload.size());
}

SendQueue::Bytes SendQueue::peek(size_t max_bytes)
{
    Sync::MutexLocker locker(m_mutex);
    Bytes result;
    auto bytes_to_send = min(max_bytes, m_stream.used_buffer_size());
    result.bytes.resize(bytes_to_send);
    m_stream.peek_some(result.bytes);
    return result;
}

void SendQueue::discard(size_t bytes_count)
{
    Sync::MutexLocker locker(m_mutex);
    MUST(m_stream.discard(bytes_count));
}

ErrorOr<NonnullOwnPtr<TransportSocketWindows>> TransportSocketWindows::from_socket(NonnullOwnPtr<Core::LocalSocket> socket)
{
    return make<TransportSocketWindows>(move(socket));
}

ErrorOr<TransportSocketWindows::Paired> TransportSocketWindows::create_paired()
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

    return Paired {
        make<TransportSocketWindows>(move(socket0)),
        TransportHandle { File::adopt_fd(fds[1]) },
    };
}

TransportSocketWindows::TransportSocketWindows(NonnullOwnPtr<Core::LocalSocket> socket)
    : m_socket(move(socket))
{
    dbgln_if(IPC_DEBUG, "TransportSocketWindows({:p}) created with socket {:p}", this, m_socket.ptr());
    initiate_wsa();

    // Set the main IPC socket to non-blocking mode for async I/O
    unsigned long non_blocking = 1;
    MUST(Core::System::ioctl(m_socket->fd().value(), FIONBIO, &non_blocking));

    MUST(Core::System::setsockopt(m_socket->fd().value(), SOL_SOCKET, SO_SNDBUF, &SOCKET_BUFFER_SIZE, sizeof(SOCKET_BUFFER_SIZE)));
    MUST(Core::System::setsockopt(m_socket->fd().value(), SOL_SOCKET, SO_RCVBUF, &SOCKET_BUFFER_SIZE, sizeof(SOCKET_BUFFER_SIZE)));

    m_send_queue = adopt_ref(*new SendQueue);

    auto make_nonblocking_socketpair = []() {
        int fds[2];
        MUST(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, fds));
        unsigned long option = 1;
        MUST(Core::System::ioctl(fds[0], FIONBIO, &option));
        MUST(Core::System::ioctl(fds[1], FIONBIO, &option));
        return Array<int, 2> { fds[0], fds[1] };
    };

    auto wakeup_fds = make_nonblocking_socketpair();
    m_wakeup_io_thread_read_fd = adopt_ref(*new AutoCloseFileDescriptor(wakeup_fds[0]));
    m_wakeup_io_thread_write_fd = adopt_ref(*new AutoCloseFileDescriptor(wakeup_fds[1]));

    auto notify_fds = make_nonblocking_socketpair();
    m_notify_hook_read_fd = adopt_ref(*new AutoCloseFileDescriptor(notify_fds[0]));
    m_notify_hook_write_fd = adopt_ref(*new AutoCloseFileDescriptor(notify_fds[1]));

    m_io_thread = Threading::Thread::construct("IPC IO"sv, [this] { return io_thread_loop(); });
    m_io_thread->start();
}

TransportSocketWindows::~TransportSocketWindows()
{
    dbgln_if(IPC_DEBUG, "TransportSocketWindows({:p}) destroyed", this);

    // During process shutdown (especially via _exit), pthread infrastructure may be torn down.
    // Try to stop the thread gracefully, but if it can't be joined safely, detach it instead.
    if (m_io_thread && m_io_thread->needs_to_be_joined()) {
        m_io_thread_state.store(IOThreadState::Stopped, AK::MemoryOrder::memory_order_release);
        wake_io_thread();

        // If the thread has already exited, we can join safely
        if (m_io_thread->has_exited()) {
            (void)m_io_thread->join();
        } else {
            // Thread hasn't exited yet. During normal shutdown this shouldn't happen,
            // but during abrupt process termination (_exit), detaching is safer than blocking.
            dbgln_if(IPC_DEBUG, "TransportSocketWindows({:p}): I/O thread hasn't exited, detaching", this);
            m_io_thread->detach();
        }
    }

    m_read_hook_notifier.clear();
    terminate_wsa();
}

void TransportSocketWindows::stop_io_thread(IOThreadState desired_state)
{
    VERIFY(desired_state == IOThreadState::Stopped || desired_state == IOThreadState::SendPendingMessagesAndStop);
    m_io_thread_state.store(desired_state, AK::MemoryOrder::memory_order_release);
    wake_io_thread();
    if (m_io_thread && m_io_thread->needs_to_be_joined())
        (void)m_io_thread->join();
}

void TransportSocketWindows::wake_io_thread()
{
    Array<u8, 1> bytes = { 0 };
    (void)Core::System::send(m_wakeup_io_thread_write_fd->value(), bytes, 0);
}

void TransportSocketWindows::notify_read_available()
{
    if (!m_notify_hook_write_fd)
        return;
    Array<u8, 1> bytes = { 0 };
    (void)Core::System::send(m_notify_hook_write_fd->value(), bytes, 0);
}

intptr_t TransportSocketWindows::io_thread_loop()
{
    dbgln_if(IPC_DEBUG, "TransportSocketWindows({:p})::io_thread_loop: starting", this);
    Array<struct pollfd, 2> pollfds;
    for (;;) {
        auto want_to_write = [&] {
            auto [bytes] = m_send_queue->peek(1);
            return !bytes.is_empty();
        }();

        auto state = m_io_thread_state.load(AK::MemoryOrder::memory_order_acquire);
        if (state == IOThreadState::Stopped)
            break;
        if (state == IOThreadState::SendPendingMessagesAndStop && !want_to_write) {
            m_io_thread_state = IOThreadState::Stopped;
            break;
        }

        short events = POLLIN;
        if (want_to_write)
            events |= POLLOUT;
        pollfds[0] = { .fd = static_cast<SOCKET>(m_socket->fd().value()), .events = events, .revents = 0 };
        pollfds[1] = { .fd = static_cast<SOCKET>(m_wakeup_io_thread_read_fd->value()), .events = POLLIN, .revents = 0 };

        auto result = WSAPoll(pollfds.data(), static_cast<ULONG>(pollfds.size()), -1);
        if (result == SOCKET_ERROR) {
            auto error = Error::from_windows_error();
            dbgln_if(IPC_DEBUG, "TransportSocketWindows poll error: {}", error);
            m_io_thread_state = IOThreadState::Stopped;
            break;
        }

        if (pollfds[1].revents & POLLIN) {
            char buf[64];
            (void)Core::System::recv(m_wakeup_io_thread_read_fd->value(), { buf, sizeof(buf) }, 0);
        }

        if (pollfds[0].revents & POLLIN)
            read_incoming_messages();

        if (m_peer_eof) {
            m_io_thread_state = IOThreadState::Stopped;
            break;
        }

        if (pollfds[0].revents & POLLHUP) {
            dbgln_if(IPC_DEBUG, "TransportSocketWindows({:p}): peer hung up (POLLHUP) in I/O thread", this);
            m_io_thread_state = IOThreadState::Stopped;
            break;
        }

        if (pollfds[0].revents & (POLLERR | POLLNVAL)) {
            dbgln_if(IPC_DEBUG, "TransportSocketWindows({:p}) I/O thread: socket error (POLLERR or POLLNVAL)", this);
            m_io_thread_state = IOThreadState::Stopped;
            break;
        }

        if (pollfds[0].revents & POLLOUT) {
            auto [bytes] = m_send_queue->peek(4096);
            if (!bytes.is_empty()) {
                ReadonlyBytes remaining = bytes;
                if (transfer_data(remaining) == TransferState::SocketClosed) {
                    m_io_thread_state = IOThreadState::Stopped;
                }
            }
        }
    }

    VERIFY(m_io_thread_state == IOThreadState::Stopped);
    dbgln_if(IPC_DEBUG, "TransportSocketWindows({:p})::io_thread_loop: exiting, state={}", this, static_cast<int>(m_io_thread_state.load()));
    if (!m_is_being_transferred.load(AK::MemoryOrder::memory_order_acquire)) {
        m_peer_eof = true;
        m_incoming_cv.broadcast();
        notify_read_available();
    }
    return 0;
}

void TransportSocketWindows::set_peer_pid(int pid)
{
    dbgln_if(IPC_DEBUG, "TransportSocketWindows({:p})::set_peer_pid({})", this, pid);
    m_peer_pid = pid;
}

void TransportSocketWindows::set_up_read_hook(Function<void()> hook)
{
    m_on_read_hook = move(hook);
    m_read_hook_notifier = Core::Notifier::construct(m_notify_hook_read_fd->value(), Core::NotificationType::Read);
    m_read_hook_notifier->on_activation = [this] {
        char buf[64];
        (void)Core::System::recv(m_notify_hook_read_fd->value(), { buf, sizeof(buf) }, 0);
        if (m_on_read_hook)
            m_on_read_hook();
    };

    {
        Sync::MutexLocker locker(m_incoming_mutex);
        if (!m_incoming_messages.is_empty()) {
            Array<u8, 1> bytes = { 0 };
            (void)Core::System::send(m_notify_hook_write_fd->value(), bytes, 0);
        }
    }
}

bool TransportSocketWindows::is_open() const
{
    return m_socket->is_open();
}

void TransportSocketWindows::close()
{
    dbgln_if(IPC_DEBUG, "TransportSocketWindows({:p})::close()", this);
    stop_io_thread(IOThreadState::Stopped);
    m_socket->close();
}

void TransportSocketWindows::close_after_sending_all_pending_messages()
{
    stop_io_thread(IOThreadState::SendPendingMessagesAndStop);
    m_socket->close();
}

void TransportSocketWindows::wait_until_readable()
{
    dbgln_if(IPC_DEBUG, "TransportSocketWindows({:p})::wait_until_readable: waiting...", this);
    Sync::MutexLocker lock(m_incoming_mutex);
    while (m_incoming_messages.is_empty() && m_io_thread_state == IOThreadState::Running) {
        m_incoming_cv.wait();
    }
    dbgln_if(IPC_DEBUG, "TransportSocketWindows({:p})::wait_until_readable: done, {} messages available, io_thread_state={}",
        this, m_incoming_messages.size(), static_cast<int>(m_io_thread_state.load()));
}

struct MessageHeader {
    u32 payload_size { 0 };
    u32 attachment_data_size { 0 };
    u32 attachment_count { 0 };
};

ErrorOr<Vector<u8>> TransportSocketWindows::serialize_attachments(Vector<Attachment>& attachments)
{
    if (attachments.is_empty())
        return Vector<u8> {};

    VERIFY(m_peer_pid != -1);

    HANDLE peer_process_handle = OpenProcess(PROCESS_DUP_HANDLE, FALSE, m_peer_pid);
    if (!peer_process_handle)
        return Error::from_windows_error();
    ScopeGuard peer_process_guard = [&] { CloseHandle(peer_process_handle); };

    Vector<u8> serialized_attachments;
    TRY(serialized_attachments.try_ensure_capacity(attachments.size() * MAX_SERIALIZED_ATTACHMENT_SIZE));

    for (auto& attachment : attachments) {
        int handle = attachment.to_fd();
        ScopeGuard close_original_handle = [&] {
            if (handle != -1)
                (void)Core::System::close(handle);
        };

        if (Core::System::is_socket(handle)) {
            TRY(serialized_attachments.try_append(to_underlying(HandleType::Socket)));

            WSAPROTOCOL_INFOW pi {};
            if (WSADuplicateSocketW(handle, m_peer_pid, &pi))
                return Error::from_windows_error();
            TRY(serialized_attachments.try_append(reinterpret_cast<u8*>(&pi), sizeof(pi)));
        } else {
            TRY(serialized_attachments.try_append(to_underlying(HandleType::Generic)));

            HANDLE duplicated_handle = INVALID_HANDLE_VALUE;
            if (!DuplicateHandle(GetCurrentProcess(), to_handle(handle), peer_process_handle, &duplicated_handle, 0, FALSE, DUPLICATE_SAME_ACCESS))
                return Error::from_windows_error();

            auto duplicated_fd = to_fd(duplicated_handle);
            TRY(serialized_attachments.try_append(reinterpret_cast<u8*>(&duplicated_fd), sizeof(duplicated_fd)));
        }
    }

    attachments.clear();
    return serialized_attachments;
}

Attachment TransportSocketWindows::deserialize_attachment(ReadonlyBytes& serialized_bytes)
{
    VERIFY(serialized_bytes.size() >= sizeof(HandleType));

    UnderlyingType<HandleType> raw_type {};
    ByteReader::load(serialized_bytes.data(), raw_type);
    auto type = static_cast<HandleType>(raw_type);
    serialized_bytes = serialized_bytes.slice(sizeof(HandleType));

    switch (type) {
    case HandleType::Generic: {
        VERIFY(serialized_bytes.size() >= sizeof(int));

        int handle = -1;
        ByteReader::load(serialized_bytes.data(), handle);
        serialized_bytes = serialized_bytes.slice(sizeof(handle));
        return Attachment::from_fd(handle);
    }
    case HandleType::Socket: {
        VERIFY(serialized_bytes.size() >= sizeof(WSAPROTOCOL_INFOW));

        WSAPROTOCOL_INFOW pi {};
        memcpy(&pi, serialized_bytes.data(), sizeof(pi));
        serialized_bytes = serialized_bytes.slice(sizeof(pi));

        // Use FROM_PROTOCOL_INFO to let WSASocket extract socket parameters from the WSAPROTOCOL_INFOW structure
        auto handle = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &pi, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
        VERIFY(handle != INVALID_SOCKET);
        return Attachment::from_fd(handle);
    }
    }

    VERIFY_NOT_REACHED();
}

void TransportSocketWindows::post_message(Vector<u8> const& bytes, Vector<Attachment>& attachments)
{
    VERIFY(bytes.size() <= MAX_MESSAGE_PAYLOAD_SIZE);
    VERIFY(attachments.size() <= MAX_MESSAGE_FD_COUNT);

    auto attachment_count = attachments.size();
    auto serialized_attachments_or_error = serialize_attachments(attachments);
    if (serialized_attachments_or_error.is_error()) {
        dbgln_if(IPC_DEBUG, "TransportSocketWindows({:p})::post_message: failed to serialize attachments: {}", this, serialized_attachments_or_error.error());
        return;
    }
    auto serialized_attachments = serialized_attachments_or_error.release_value();
    VERIFY(serialized_attachments.size() <= MAX_ATTACHMENT_DATA_SIZE);

    MessageHeader header {
        .payload_size = static_cast<u32>(bytes.size()),
        .attachment_data_size = static_cast<u32>(serialized_attachments.size()),
        .attachment_count = static_cast<u32>(attachment_count),
    };

    dbgln_if(IPC_DEBUG, "TransportSocketWindows({:p})::post_message: enqueuing {} bytes, {} attachments", this, bytes.size(), attachment_count);
    m_send_queue->enqueue_message({ reinterpret_cast<u8 const*>(&header), sizeof(header) }, serialized_attachments, bytes);

    wake_io_thread();
}

TransportSocketWindows::TransferState TransportSocketWindows::transfer_data(ReadonlyBytes& bytes)
{
    auto byte_count = bytes.size();
    auto maybe_nwritten = m_socket->write_some(bytes);

    if (maybe_nwritten.is_error()) {
        auto error = maybe_nwritten.release_error();
        if (error.code() == WSAEWOULDBLOCK)
            return TransferState::Continue;
        if (error.code() == WSAECONNABORTED || error.code() == WSAECONNRESET) {
            dbgln_if(IPC_DEBUG, "TransportSocketWindows({:p}): peer hung up", this);
            return TransferState::SocketClosed;
        }

        dbgln_if(IPC_DEBUG, "TransportSocketWindows::transfer_data error: {}", error);
        return TransferState::SocketClosed;
    }

    bytes = bytes.slice(maybe_nwritten.value());
    auto written_byte_count = byte_count - bytes.size();
    if (written_byte_count > 0)
        m_send_queue->discard(written_byte_count);

    return TransferState::Continue;
}

void TransportSocketWindows::read_incoming_messages()
{
    Vector<NonnullOwnPtr<Message>> batch;
    while (m_socket->is_open()) {
        u8 buffer[4096];
        auto maybe_bytes_read = m_socket->read_without_waiting({ buffer, sizeof(buffer) });

        if (maybe_bytes_read.is_error()) {
            auto error = maybe_bytes_read.release_error();
            if (error.code() == EWOULDBLOCK)
                break;
            if (error.code() == ECONNRESET) {
                dbgln_if(IPC_DEBUG, "TransportSocketWindows({:p}): peer reset connection", this);
                m_peer_eof = true;
                break;
            }
            dbgln_if(IPC_DEBUG, "TransportSocketWindows::read_incoming_messages: {}", error);
            m_peer_eof = true;
            break;
        }

        auto bytes_read = maybe_bytes_read.release_value();
        if (bytes_read.is_empty()) {
            dbgln_if(IPC_DEBUG, "TransportSocketWindows({:p}): peer EOF", this);
            m_peer_eof = true;
            break;
        }

        if (m_unprocessed_bytes.size() + bytes_read.size() > 128 * MiB) {
            dbgln_if(IPC_DEBUG, "TransportSocketWindows: Unprocessed buffer too large, disconnecting peer");
            m_peer_eof = true;
            break;
        }
        if (m_unprocessed_bytes.try_append(bytes_read.data(), bytes_read.size()).is_error()) {
            dbgln_if(IPC_DEBUG, "TransportSocketWindows: Failed to append to unprocessed_bytes buffer");
            m_peer_eof = true;
            break;
        }
    }

    size_t index = 0;
    while (index + sizeof(MessageHeader) <= m_unprocessed_bytes.size()) {
        MessageHeader header;
        memcpy(&header, m_unprocessed_bytes.data() + index, sizeof(MessageHeader));

        if (header.payload_size > MAX_MESSAGE_PAYLOAD_SIZE) {
            dbgln_if(IPC_DEBUG, "TransportSocketWindows: Rejecting message with payload_size {} exceeding limit {}", header.payload_size, MAX_MESSAGE_PAYLOAD_SIZE);
            m_peer_eof = true;
            break;
        }

        if (header.attachment_data_size > MAX_ATTACHMENT_DATA_SIZE) {
            dbgln_if(IPC_DEBUG, "TransportSocketWindows: Rejecting message with attachment_data_size {} exceeding limit {}", header.attachment_data_size, MAX_ATTACHMENT_DATA_SIZE);
            m_peer_eof = true;
            break;
        }

        Checked<size_t> message_size = header.payload_size;
        message_size += header.attachment_data_size;
        message_size += sizeof(MessageHeader);

        if (message_size.has_overflow() || message_size.value() > m_unprocessed_bytes.size() - index)
            break;

        auto message = make<Message>();
        auto attachment_bytes = ReadonlyBytes { m_unprocessed_bytes.data() + index + sizeof(MessageHeader), header.attachment_data_size };
        for (u32 i = 0; i < header.attachment_count; ++i)
            message->attachments.enqueue(deserialize_attachment(attachment_bytes));

        auto const* payload = m_unprocessed_bytes.data() + index + sizeof(MessageHeader) + header.attachment_data_size;
        if (message->bytes.try_append(payload, header.payload_size).is_error()) {
            dbgln_if(IPC_DEBUG, "TransportSocketWindows: Failed to allocate message buffer for payload_size {}", header.payload_size);
            m_peer_eof = true;
            break;
        }

        batch.append(move(message));
        index += message_size.value();
    }

    if (index > 0) {
        if (index < m_unprocessed_bytes.size()) {
            auto remaining = m_unprocessed_bytes.size() - index;
            m_unprocessed_bytes.overwrite(0, m_unprocessed_bytes.data() + index, remaining);
            m_unprocessed_bytes.resize(remaining);
        } else {
            m_unprocessed_bytes.clear();
        }
    }

    if (!batch.is_empty()) {
        Sync::MutexLocker locker(m_incoming_mutex);
        dbgln_if(IPC_DEBUG, "TransportSocketWindows({:p})::read_incoming_messages: adding {} messages to queue", this, batch.size());
        m_incoming_messages.extend(move(batch));
        m_incoming_cv.broadcast();
        notify_read_available();
    }

    if (m_peer_eof) {
        m_incoming_cv.broadcast();
        notify_read_available();
    }
}

TransportSocketWindows::ShouldShutdown TransportSocketWindows::read_as_many_messages_as_possible_without_blocking(Function<void(Message&&)>&& callback)
{
    Vector<NonnullOwnPtr<Message>> messages;
    {
        Sync::MutexLocker locker(m_incoming_mutex);
        messages = move(m_incoming_messages);
    }
    for (auto& message : messages)
        callback(move(*message));
    return m_peer_eof ? ShouldShutdown::Yes : ShouldShutdown::No;
}

ErrorOr<TransportHandle> TransportSocketWindows::release_for_transfer()
{
    m_is_being_transferred.store(true, AK::MemoryOrder::memory_order_release);
    stop_io_thread(IOThreadState::SendPendingMessagesAndStop);
    auto fd = TRY(m_socket->release_fd());
    return TransportHandle { File::adopt_fd(fd) };
}

}
