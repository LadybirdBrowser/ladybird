/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteReader.h>
#include <AK/Checked.h>
#include <AK/Types.h>
#include <LibIPC/HandleType.h>
#include <LibIPC/Limits.h>
#include <LibIPC/TransportSocketWindows.h>

#include <AK/Windows.h>

namespace IPC {

TransportSocketWindows::TransportSocketWindows(NonnullOwnPtr<Core::LocalSocket> socket)
    : m_socket(move(socket))
{
}

void TransportSocketWindows::set_peer_pid(int pid)
{
    m_peer_pid = pid;
}

void TransportSocketWindows::set_up_read_hook(Function<void()> hook)
{
    VERIFY(m_socket->is_open());
    m_socket->on_ready_to_read = move(hook);
}

bool TransportSocketWindows::is_open() const
{
    return m_socket->is_open();
}

void TransportSocketWindows::close()
{
    m_socket->close();
}

void TransportSocketWindows::close_after_sending_all_pending_messages()
{
    close();
}

void TransportSocketWindows::wait_until_readable()
{
    auto readable = MUST(m_socket->can_read_without_blocking(-1));
    VERIFY(readable);
}

ErrorOr<void> TransportSocketWindows::duplicate_handles(Bytes bytes, Vector<size_t> const& handle_offsets)
{
    if (handle_offsets.is_empty())
        return {};

    if (m_peer_pid == -1)
        return Error::from_string_literal("Transport is not initialized");

    HANDLE peer_process_handle = OpenProcess(PROCESS_DUP_HANDLE, FALSE, m_peer_pid);
    if (!peer_process_handle)
        return Error::from_windows_error();
    ScopeGuard guard = [&] { CloseHandle(peer_process_handle); };

    for (auto offset : handle_offsets) {

        auto span = bytes.slice(offset);
        if (span.size() < sizeof(HandleType))
            return Error::from_string_literal("Not enough bytes");

        UnderlyingType<HandleType> raw_type {};
        ByteReader::load(span.data(), raw_type);
        auto type = static_cast<HandleType>(raw_type);
        if (type != HandleType::Generic && type != HandleType::Socket)
            return Error::from_string_literal("Invalid handle type");
        span = span.slice(sizeof(HandleType));

        if (type == HandleType::Socket) {
            if (span.size() < sizeof(WSAPROTOCOL_INFOW))
                return Error::from_string_literal("Not enough bytes for socket handle");

            // We stashed the bytes of this process's version of the handle at the offset location
            int handle = -1;
            ByteReader::load(span.data(), handle);

            auto* pi = reinterpret_cast<WSAPROTOCOL_INFOW*>(span.data());
            if (WSADuplicateSocketW(handle, m_peer_pid, pi))
                return Error::from_windows_error();
        } else {
            if (span.size() < sizeof(int))
                return Error::from_string_literal("Not enough bytes for generic handle");

            int handle = -1;
            ByteReader::load(span.data(), handle);

            HANDLE new_handle = INVALID_HANDLE_VALUE;
            if (!DuplicateHandle(GetCurrentProcess(), to_handle(handle), peer_process_handle, &new_handle, 0, FALSE, DUPLICATE_SAME_ACCESS))
                return Error::from_windows_error();

            ByteReader::store(span.data(), to_fd(new_handle));
        }
    }

    return {};
}

// Maximum size of accumulated unprocessed bytes before we disconnect the peer
static constexpr size_t MAX_UNPROCESSED_BUFFER_SIZE = 128 * MiB;

struct MessageHeader {
    u32 size { 0 };
};

ErrorOr<void> TransportSocketWindows::transfer_message(ReadonlyBytes bytes, Vector<size_t> const& handle_offsets)
{
    Vector<u8> message_buffer;
    message_buffer.resize(sizeof(MessageHeader) + bytes.size());
    MessageHeader header;
    header.size = bytes.size();
    memcpy(message_buffer.data(), &header, sizeof(MessageHeader));
    memcpy(message_buffer.data() + sizeof(MessageHeader), bytes.data(), bytes.size());

    TRY(duplicate_handles({ message_buffer.data() + sizeof(MessageHeader), bytes.size() }, handle_offsets));

    return transfer(message_buffer.span());
}

ErrorOr<void> TransportSocketWindows::transfer(ReadonlyBytes bytes_to_write)
{
    while (!bytes_to_write.is_empty()) {

        ErrorOr<size_t> maybe_nwritten = m_socket->write_some(bytes_to_write);

        if (maybe_nwritten.is_error()) {
            auto error = maybe_nwritten.release_error();
            if (error.code() != EWOULDBLOCK)
                return error;

            struct pollfd pollfd = {
                .fd = static_cast<SOCKET>(m_socket->fd().value()),
                .events = POLLOUT,
                .revents = 0
            };

            auto result = WSAPoll(&pollfd, 1, -1);
            if (result == 1)
                continue;
            if (result == SOCKET_ERROR)
                return Error::from_windows_error();
            dbgln("TransportSocketWindows::transfer: Unexpected WSAPoll result {}", result);
            return Error::from_string_literal("Unexpected WSAPoll result");
        }

        bytes_to_write = bytes_to_write.slice(maybe_nwritten.value());
    }
    return {};
}

TransportSocketWindows::ShouldShutdown TransportSocketWindows::read_as_many_messages_as_possible_without_blocking(Function<void(Message&&)>&& callback)
{
    auto should_shutdown = ShouldShutdown::No;

    while (is_open()) {

        u8 buffer[4096];
        auto maybe_bytes_read = m_socket->read_without_waiting({ buffer, sizeof(buffer) });

        if (maybe_bytes_read.is_error()) {
            auto error = maybe_bytes_read.release_error();
            if (error.code() == EWOULDBLOCK)
                break;
            if (error.code() == ECONNRESET) {
                should_shutdown = ShouldShutdown::Yes;
                break;
            }
            dbgln("TransportSocketWindows::read_as_many_messages_as_possible_without_blocking: {}", error);
            should_shutdown = ShouldShutdown::Yes;
            break;
        }

        auto bytes_read = maybe_bytes_read.release_value();
        if (bytes_read.is_empty()) {
            should_shutdown = ShouldShutdown::Yes;
            break;
        }

        if (m_unprocessed_bytes.size() + bytes_read.size() > MAX_UNPROCESSED_BUFFER_SIZE) {
            dbgln("TransportSocketWindows: Unprocessed buffer would exceed {} bytes, disconnecting peer", MAX_UNPROCESSED_BUFFER_SIZE);
            should_shutdown = ShouldShutdown::Yes;
            break;
        }
        if (m_unprocessed_bytes.try_append(bytes_read.data(), bytes_read.size()).is_error()) {
            dbgln("TransportSocketWindows: Failed to append to unprocessed_bytes buffer");
            should_shutdown = ShouldShutdown::Yes;
            break;
        }
    }

    size_t index = 0;
    while (index + sizeof(MessageHeader) <= m_unprocessed_bytes.size()) {
        MessageHeader header;
        memcpy(&header, m_unprocessed_bytes.data() + index, sizeof(MessageHeader));
        if (header.size > MAX_MESSAGE_PAYLOAD_SIZE) {
            dbgln("TransportSocketWindows: Rejecting message with size {} exceeding limit {}", header.size, MAX_MESSAGE_PAYLOAD_SIZE);
            should_shutdown = ShouldShutdown::Yes;
            break;
        }
        Checked<size_t> message_size = header.size;
        message_size += sizeof(MessageHeader);
        if (message_size.has_overflow() || message_size.value() > m_unprocessed_bytes.size() - index)
            break;
        Message message;
        if (message.bytes.try_append(m_unprocessed_bytes.data() + index + sizeof(MessageHeader), header.size).is_error()) {
            dbgln("TransportSocketWindows: Failed to allocate message buffer for size {}", header.size);
            should_shutdown = ShouldShutdown::Yes;
            break;
        }
        callback(move(message));
        Checked<size_t> new_index = index;
        new_index += header.size;
        new_index += sizeof(MessageHeader);
        if (new_index.has_overflow()) {
            dbgln("TransportSocketWindows: index would overflow");
            should_shutdown = ShouldShutdown::Yes;
            break;
        }
        index = new_index.value();
    }

    if (index < m_unprocessed_bytes.size()) {
        auto remaining_bytes_or_error = ByteBuffer::copy(m_unprocessed_bytes.span().slice(index));
        if (remaining_bytes_or_error.is_error()) {
            dbgln("TransportSocketWindows: Failed to copy remaining bytes");
            should_shutdown = ShouldShutdown::Yes;
        } else {
            m_unprocessed_bytes = remaining_bytes_or_error.release_value();
        }
    } else {
        m_unprocessed_bytes.clear();
    }

    return should_shutdown;
}

ErrorOr<int> TransportSocketWindows::release_underlying_transport_for_transfer()
{
    return m_socket->release_fd();
}

ErrorOr<IPC::File> TransportSocketWindows::clone_for_transfer()
{
    return IPC::File::clone_fd(m_socket->fd().value());
}

}
