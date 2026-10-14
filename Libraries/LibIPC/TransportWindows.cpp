/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, stasoid <stasoid@yahoo.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/NumericLimits.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Notifier.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibIPC/File.h>
#include <LibIPC/Limits.h>
#include <LibIPC/TransportHandle.h>
#include <LibIPC/TransportWindows.h>

#include <AK/Windows.h>

namespace IPC {

// A message with attachments carries the handle broker token that names them right after its header. A section
// holding the payload is the last of those attachments.
struct TransportWindows::MessageHeader {
    enum class PayloadLocation : u8 {
        Inline = 0,
        SharedMemory = 1,
    };
    u32 payload_size { 0 };
    u32 attachment_count { 0 };
    PayloadLocation payload_location { PayloadLocation::Inline };

    size_t token_size() const { return attachment_count > 0 ? sizeof(HandleBrokerToken) : 0; }
    size_t inline_payload_size() const { return payload_location == PayloadLocation::Inline ? payload_size : 0; }
};

// Maximum size of accumulated unprocessed bytes before we disconnect the peer
static constexpr size_t MAX_UNPROCESSED_BUFFER_SIZE = 128 * MiB;

// A send that cannot complete at once waits in slices this long, so that closing the transport ends the wait.
static constexpr DWORD SEND_WAIT_SLICE_MS = 100;

class OverlappedOperation {
    AK_MAKE_NONCOPYABLE(OverlappedOperation);
    AK_MAKE_NONMOVABLE(OverlappedOperation);

public:
    OverlappedOperation()
    {
        m_overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        VERIFY(m_overlapped.hEvent);
    }

    ~OverlappedOperation()
    {
        CloseHandle(m_overlapped.hEvent);
    }

    OVERLAPPED* overlapped() { return &m_overlapped; }
    HANDLE event() const { return m_overlapped.hEvent; }

private:
    OVERLAPPED m_overlapped {};
};

ErrorOr<TransportWindows::Paired> TransportWindows::create_paired()
{
    auto handles = TRY(Core::System::duplex_pipe());
    ArmedScopeGuard guard_remote { [&] { MUST(Core::System::close(handles[1])); } };
    auto local = TRY(adopt_fd(handles[0]));
    guard_remote.disarm();
    return Paired { move(local), TransportHandle { File::adopt_fd(handles[1]) } };
}

ErrorOr<NonnullOwnPtr<TransportWindows>> TransportWindows::from_socket(NonnullOwnPtr<Core::LocalSocket> socket)
{
    socket->set_notifications_enabled(false);
    return adopt_fd(TRY(socket->release_fd()));
}

ErrorOr<NonnullOwnPtr<TransportWindows>> TransportWindows::adopt_fd(int handle)
{
    if (handle == -1)
        return Error::from_errno(EBADF);
    auto kind = Core::System::is_socket(handle) ? Kind::Socket : Kind::Pipe;
    if (kind == Kind::Socket) {
        u_long non_blocking = 1;
        if (ioctlsocket(handle, FIONBIO, &non_blocking) == SOCKET_ERROR)
            return Error::from_windows_error();
    }
    return adopt_own(*new TransportWindows(handle, kind));
}

TransportWindows::TransportWindows(int handle, Kind kind)
    : m_handle(handle)
    , m_kind(kind)
    , m_handle_broker_channel(get_random<HandleBrokerChannel>())
{
}

TransportWindows::~TransportWindows()
{
    close();
}

void TransportWindows::set_up_read_hook(Function<void()> hook)
{
    VERIFY(is_open());
    m_on_read_hook = move(hook);
    m_read_notifier = Core::Notifier::construct(m_handle, Core::NotificationType::Read);
    m_read_notifier->on_activation = [this] {
        if (m_on_read_hook)
            m_on_read_hook();
    };
}

bool TransportWindows::is_open() const
{
    return m_is_open.load(AK::MemoryOrder::memory_order_relaxed) && !m_peer_has_hung_up.load(AK::MemoryOrder::memory_order_relaxed);
}

static bool send_error_means_peer_has_hung_up(Error const& error)
{
    if (!error.is_windows_error())
        return false;
    switch (error.code()) {
    case ERROR_NO_DATA:
    case ERROR_BROKEN_PIPE:
    case ERROR_PIPE_NOT_CONNECTED:
    case WSAECONNRESET:
    case WSAECONNABORTED:
    case WSAESHUTDOWN:
    case WSAENOTCONN:
        return true;
    default:
        return false;
    }
}

// The notifier has a read of its own in flight on a pipe, which has to end before the handle may go anywhere.
void TransportWindows::stop_watching()
{
    if (auto notifier = move(m_read_notifier))
        notifier->close();
}

void TransportWindows::close()
{
    m_is_open.store(false, AK::MemoryOrder::memory_order_relaxed);
    MutexLocker locker(m_send_mutex);
    stop_watching();
    if (m_handle != -1) {
        (void)Core::System::close(m_handle);
        m_handle = -1;
    }
}

void TransportWindows::close_after_sending_all_pending_messages()
{
    close();
}

void TransportWindows::wait_until_readable()
{
    if (m_kind == Kind::Socket) {
        WSAPOLLFD poll_fd {
            .fd = static_cast<SOCKET>(m_handle),
            .events = POLLRDNORM,
            .revents = 0,
        };
        auto result = WSAPoll(&poll_fd, 1, -1);
        VERIFY(result != SOCKET_ERROR);
        return;
    }

    // A zero-byte read completes once there is something to read, or once the pipe is broken.
    OverlappedOperation operation;
    static u8 s_zero_byte_buffer;
    if (ReadFile(to_handle(m_handle), &s_zero_byte_buffer, 0, nullptr, operation.overlapped()))
        return;
    if (GetLastError() != ERROR_IO_PENDING)
        return;
    WaitForSingleObject(operation.event(), INFINITE);
}

ErrorOr<size_t> TransportWindows::write_some(ReadonlyBytes bytes)
{
    if (m_kind == Kind::Socket) {
        auto sent = ::send(static_cast<SOCKET>(m_handle), reinterpret_cast<char const*>(bytes.data()), static_cast<int>(min(bytes.size(), static_cast<size_t>(NumericLimits<int>::max()))), 0);
        if (sent == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK)
                return Error::from_errno(EWOULDBLOCK);
            return Error::from_windows_error();
        }
        return static_cast<size_t>(sent);
    }

    // A pipe write completes only once every byte is in the pipe, so it is waited for in slices that notice a close.
    OverlappedOperation operation;
    DWORD bytes_written = 0;
    auto size = static_cast<DWORD>(min(bytes.size(), static_cast<size_t>(NumericLimits<DWORD>::max())));
    if (WriteFile(to_handle(m_handle), bytes.data(), size, &bytes_written, operation.overlapped()))
        return static_cast<size_t>(bytes_written);
    if (GetLastError() != ERROR_IO_PENDING)
        return Error::from_windows_error();

    for (;;) {
        if (m_disconnects_when_peer_stops_reading || !is_open()) {
            CancelIoEx(to_handle(m_handle), operation.overlapped());
            GetOverlappedResult(to_handle(m_handle), operation.overlapped(), &bytes_written, TRUE);
            if (!is_open())
                return Error::from_string_literal("Transport closed during send");
            return Error::from_string_literal("Peer has stopped reading");
        }
        if (WaitForSingleObject(operation.event(), SEND_WAIT_SLICE_MS) == WAIT_OBJECT_0)
            break;
    }
    if (!GetOverlappedResult(to_handle(m_handle), operation.overlapped(), &bytes_written, FALSE))
        return Error::from_windows_error();
    return static_cast<size_t>(bytes_written);
}

ErrorOr<void> TransportWindows::transfer(ReadonlyBytes bytes_to_write)
{
    while (!bytes_to_write.is_empty()) {
        if (!is_open())
            return Error::from_string_literal("Transport closed during send");

        auto written = write_some(bytes_to_write);
        if (written.is_error()) {
            auto error = written.release_error();
            if (!error.is_errno() || error.code() != EWOULDBLOCK) {
                if (send_error_means_peer_has_hung_up(error))
                    m_peer_has_hung_up.store(true, AK::MemoryOrder::memory_order_relaxed);
                return error;
            }
            if (m_disconnects_when_peer_stops_reading)
                return Error::from_string_literal("Peer has stopped reading");

            WSAPOLLFD poll_fd {
                .fd = static_cast<SOCKET>(m_handle),
                .events = POLLWRNORM,
                .revents = 0,
            };
            if (WSAPoll(&poll_fd, 1, SEND_WAIT_SLICE_MS) == SOCKET_ERROR)
                return Error::from_windows_error();
            continue;
        }
        bytes_to_write = bytes_to_write.slice(written.value());
    }
    return {};
}

static ErrorOr<Attachment> copy_payload_to_shared_memory(ReadonlyBytes payload)
{
    auto section = TRY(Core::AnonymousBuffer::create_with_size(payload.size()));
    payload.copy_to({ section.data<u8>(), section.size() });
    return Attachment::from_fd(TRY(Core::System::dup(section.fd())));
}

static ErrorOr<Vector<u8>> copy_payload_from_shared_memory(Attachment section, size_t size)
{
    auto buffer = TRY(Core::AnonymousBuffer::create_from_anon_fd(section.to_fd(), size));
    Vector<u8> payload;
    TRY(payload.try_append(buffer.bytes().data(), buffer.size()));
    return payload;
}

ErrorOr<void> TransportWindows::post_message(MessageDataType bytes, Vector<Attachment>& attachments)
{
    VERIFY(bytes.size() <= MAX_MESSAGE_PAYLOAD_SIZE);
    VERIFY(attachments.size() <= MAX_MESSAGE_FD_COUNT);

    MutexLocker locker(m_send_mutex);

    MessageHeader header {
        .payload_size = static_cast<u32>(bytes.size()),
        .attachment_count = static_cast<u32>(attachments.size()),
    };
    if (bytes.size() > MAX_INLINE_PAYLOAD_SIZE) {
        attachments.append(TRY(copy_payload_to_shared_memory(bytes)));
        bytes.clear();
        header.payload_location = MessageHeader::PayloadLocation::SharedMemory;
        ++header.attachment_count;
    }
    HandleBrokerToken attachment_token = 0;
    if (!attachments.is_empty()) {
        attachment_token = TRY(HandleBroker::the().park(m_handle_broker_channel, move(attachments)));
        attachments.clear();
        m_has_parked_attachments = true;
    }

    Vector<u8> message_buffer;
    TRY(message_buffer.try_resize(sizeof(MessageHeader) + header.token_size() + bytes.size()));
    memcpy(message_buffer.data(), &header, sizeof(header));
    if (header.token_size() > 0)
        memcpy(message_buffer.data() + sizeof(MessageHeader), &attachment_token, sizeof(attachment_token));
    if (!bytes.is_empty())
        memcpy(message_buffer.data() + sizeof(MessageHeader) + header.token_size(), bytes.data(), bytes.size());

    return transfer(message_buffer.span());
}

TransportWindows::ReadOutcome TransportWindows::read_some_without_waiting(Bytes buffer, size_t& bytes_read)
{
    bytes_read = 0;
    if (m_kind == Kind::Socket) {
        WSAPOLLFD poll_fd {
            .fd = static_cast<SOCKET>(m_handle),
            .events = POLLRDNORM,
            .revents = 0,
        };
        auto ready = WSAPoll(&poll_fd, 1, 0);
        if (ready == 0)
            return ReadOutcome::WouldBlock;
        if (ready == SOCKET_ERROR)
            return ReadOutcome::Failed;
        auto received = ::recv(static_cast<SOCKET>(m_handle), reinterpret_cast<char*>(buffer.data()), static_cast<int>(min(buffer.size(), static_cast<size_t>(NumericLimits<int>::max()))), 0);
        if (received == 0)
            return ReadOutcome::EndOfStream;
        if (received == SOCKET_ERROR) {
            auto error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK)
                return ReadOutcome::WouldBlock;
            if (error == WSAECONNRESET || error == WSAECONNABORTED)
                return ReadOutcome::EndOfStream;
            dbgln("TransportWindows: recv failed: {}", Error::from_windows_error(error));
            return ReadOutcome::Failed;
        }
        bytes_read = static_cast<size_t>(received);
        return ReadOutcome::Read;
    }

    DWORD bytes_available = 0;
    if (!PeekNamedPipe(to_handle(m_handle), nullptr, 0, nullptr, &bytes_available, nullptr)) {
        auto error = GetLastError();
        if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA)
            return ReadOutcome::EndOfStream;
        dbgln("TransportWindows: PeekNamedPipe failed: {}", Error::from_windows_error(error));
        return ReadOutcome::Failed;
    }
    if (bytes_available == 0)
        return ReadOutcome::WouldBlock;

    // What is there to read is read at once, so this never waits.
    OverlappedOperation operation;
    DWORD read_now = 0;
    auto size = static_cast<DWORD>(min(buffer.size(), static_cast<size_t>(bytes_available)));
    if (!ReadFile(to_handle(m_handle), buffer.data(), size, &read_now, operation.overlapped())) {
        auto error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            if (!GetOverlappedResult(to_handle(m_handle), operation.overlapped(), &read_now, TRUE))
                error = GetLastError();
            else
                error = ERROR_SUCCESS;
        }
        if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED)
            return ReadOutcome::EndOfStream;
        if (error != ERROR_SUCCESS) {
            dbgln("TransportWindows: ReadFile failed: {}", Error::from_windows_error(error));
            return ReadOutcome::Failed;
        }
    }
    bytes_read = read_now;
    return ReadOutcome::Read;
}

TransportWindows::ShouldShutdown TransportWindows::read_as_many_messages_as_possible_without_blocking(Function<void(Message&&)>&& callback)
{
    auto should_shutdown = ShouldShutdown::No;

    // What the peer sent before hanging up is still here to be read, so only this end closing stops the reading.
    while (m_is_open.load(AK::MemoryOrder::memory_order_relaxed)) {
        u8 buffer[4096];
        size_t bytes_read = 0;
        auto outcome = read_some_without_waiting({ buffer, sizeof(buffer) }, bytes_read);
        if (outcome == ReadOutcome::WouldBlock)
            break;
        if (outcome != ReadOutcome::Read) {
            should_shutdown = ShouldShutdown::Yes;
            break;
        }

        if (m_unprocessed_bytes.size() + bytes_read > MAX_UNPROCESSED_BUFFER_SIZE) {
            dbgln("TransportWindows: Unprocessed buffer would exceed {} bytes, disconnecting peer", MAX_UNPROCESSED_BUFFER_SIZE);
            should_shutdown = ShouldShutdown::Yes;
            break;
        }
        if (m_unprocessed_bytes.try_append(buffer, bytes_read).is_error()) {
            dbgln("TransportWindows: Failed to append to unprocessed_bytes buffer");
            should_shutdown = ShouldShutdown::Yes;
            break;
        }
    }

    size_t index = 0;
    while (index + sizeof(MessageHeader) <= m_unprocessed_bytes.size()) {
        MessageHeader header;
        memcpy(&header, m_unprocessed_bytes.data() + index, sizeof(MessageHeader));
        if (header.payload_location != MessageHeader::PayloadLocation::Inline && header.payload_location != MessageHeader::PayloadLocation::SharedMemory) {
            dbgln("TransportWindows: Rejecting message with unknown payload location {}", to_underlying(header.payload_location));
            should_shutdown = ShouldShutdown::Yes;
            break;
        }
        bool payload_is_in_shared_memory = header.payload_location == MessageHeader::PayloadLocation::SharedMemory;
        if (!payload_is_in_shared_memory && header.payload_size > MAX_INLINE_PAYLOAD_SIZE) {
            dbgln("TransportWindows: Rejecting message with payload_size {} exceeding limit {}", header.payload_size, MAX_INLINE_PAYLOAD_SIZE);
            should_shutdown = ShouldShutdown::Yes;
            break;
        }
        auto max_attachment_count = payload_is_in_shared_memory ? MAX_MESSAGE_FD_COUNT_WITH_SHARED_MEMORY : MAX_MESSAGE_FD_COUNT;
        if (header.attachment_count > max_attachment_count) {
            dbgln("TransportWindows: Rejecting message with attachment_count {} exceeding limit {}", header.attachment_count, max_attachment_count);
            should_shutdown = ShouldShutdown::Yes;
            break;
        }
        if (payload_is_in_shared_memory && header.attachment_count == 0) {
            dbgln("TransportWindows: Rejecting shared memory message without a section");
            should_shutdown = ShouldShutdown::Yes;
            break;
        }

        Checked<size_t> message_size = header.inline_payload_size();
        message_size += sizeof(MessageHeader);
        message_size += header.token_size();
        if (message_size.has_overflow() || message_size.value() > m_unprocessed_bytes.size() - index)
            break;

        Message message;
        Attachment section;
        if (header.attachment_count > 0) {
            HandleBrokerToken attachment_token = 0;
            memcpy(&attachment_token, m_unprocessed_bytes.data() + index + sizeof(MessageHeader), sizeof(attachment_token));
            auto attachments = HandleBroker::the().redeem(attachment_token);
            if (attachments.is_error() || attachments.value().size() != header.attachment_count) {
                dbgln("TransportWindows: Rejecting message whose attachments the handle broker does not hold");
                should_shutdown = ShouldShutdown::Yes;
                break;
            }
            if (payload_is_in_shared_memory)
                section = attachments.value().take_last();
            for (auto& attachment : attachments.value())
                message.attachments.enqueue(move(attachment));
        }

        auto const* payload = m_unprocessed_bytes.data() + index + sizeof(MessageHeader) + header.token_size();
        Vector<u8> payload_bytes;
        if (payload_is_in_shared_memory) {
            auto copied_bytes = copy_payload_from_shared_memory(move(section), header.payload_size);
            if (copied_bytes.is_error()) {
                dbgln("TransportWindows: Failed to copy payload_size {} from shared memory: {}", header.payload_size, copied_bytes.error());
                should_shutdown = ShouldShutdown::Yes;
                break;
            }
            payload_bytes = copied_bytes.release_value();
        } else if (payload_bytes.try_append(payload, header.payload_size).is_error()) {
            dbgln("TransportWindows: Failed to allocate message buffer for payload_size {}", header.payload_size);
            should_shutdown = ShouldShutdown::Yes;
            break;
        }
        message.bytes = ReceivedMessageBytes::from_vector(move(payload_bytes));
        callback(move(message));
        index = message_size.value() + index;
    }

    if (index < m_unprocessed_bytes.size()) {
        auto remaining_bytes_or_error = ByteBuffer::copy(m_unprocessed_bytes.span().slice(index));
        if (remaining_bytes_or_error.is_error()) {
            dbgln("TransportWindows: Failed to copy remaining bytes");
            should_shutdown = ShouldShutdown::Yes;
        } else {
            m_unprocessed_bytes = remaining_bytes_or_error.release_value();
        }
    } else {
        m_unprocessed_bytes.clear();
    }

    // Nothing the peer has not read by now will ever be read, so the handles that were waiting for it can go.
    if (should_shutdown == ShouldShutdown::Yes && m_has_parked_attachments) {
        HandleBroker::the().forget_channel(m_handle_broker_channel);
        m_has_parked_attachments = false;
    }

    return should_shutdown;
}

ErrorOr<TransportHandle> TransportWindows::release_for_transfer()
{
    m_is_open.store(false, AK::MemoryOrder::memory_order_relaxed);
    MutexLocker locker(m_send_mutex);
    stop_watching();
    if (m_handle == -1)
        return Error::from_errno(ENOTCONN);
    return TransportHandle { File::adopt_fd(exchange(m_handle, -1)) };
}

}
