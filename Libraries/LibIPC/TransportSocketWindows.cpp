/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteReader.h>
#include <AK/Checked.h>
#include <AK/ScopeGuard.h>
#include <AK/Types.h>
#include <LibCore/System.h>
#include <LibIPC/File.h>
#include <LibIPC/HandleType.h>
#include <LibIPC/Limits.h>
#include <LibIPC/TransportHandle.h>
#include <LibIPC/TransportSocketWindows.h>

#include <AK/Windows.h>

namespace IPC {

static constexpr size_t MAX_SERIALIZED_ATTACHMENT_SIZE = sizeof(HandleType) + sizeof(WSAPROTOCOL_INFOW);
static constexpr size_t MAX_ATTACHMENT_DATA_SIZE = MAX_MESSAGE_FD_COUNT * MAX_SERIALIZED_ATTACHMENT_SIZE;

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

// Maximum size of accumulated unprocessed bytes before we disconnect the peer
static constexpr size_t MAX_UNPROCESSED_BUFFER_SIZE = 128 * MiB;

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

        auto handle = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, &pi, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
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
    auto serialized_attachments = MUST(serialize_attachments(attachments));
    VERIFY(serialized_attachments.size() <= MAX_ATTACHMENT_DATA_SIZE);

    Vector<u8> message_buffer;
    MUST(message_buffer.try_resize(sizeof(MessageHeader) + serialized_attachments.size() + bytes.size()));

    MessageHeader header {
        .payload_size = static_cast<u32>(bytes.size()),
        .attachment_data_size = static_cast<u32>(serialized_attachments.size()),
        .attachment_count = static_cast<u32>(attachment_count),
    };
    memcpy(message_buffer.data(), &header, sizeof(header));

    auto* serialized_attachment_storage = message_buffer.data() + sizeof(MessageHeader);
    if (!serialized_attachments.is_empty())
        memcpy(serialized_attachment_storage, serialized_attachments.data(), serialized_attachments.size());

    auto* payload_storage = serialized_attachment_storage + serialized_attachments.size();
    if (!bytes.is_empty())
        memcpy(payload_storage, bytes.data(), bytes.size());

    MUST(transfer(message_buffer.span()));
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
        VERIFY(header.payload_size <= MAX_MESSAGE_PAYLOAD_SIZE);
        VERIFY(header.attachment_count <= MAX_MESSAGE_FD_COUNT);
        VERIFY(header.attachment_data_size <= MAX_ATTACHMENT_DATA_SIZE);

        Checked<size_t> message_size = header.payload_size;
        message_size += header.attachment_data_size;
        message_size += sizeof(MessageHeader);
        if (message_size.has_overflow() || message_size.value() > m_unprocessed_bytes.size() - index)
            break;
        Message message;
        auto attachment_bytes = ReadonlyBytes { m_unprocessed_bytes.data() + index + sizeof(MessageHeader), header.attachment_data_size };
        for (u32 attachment_index = 0; attachment_index < header.attachment_count; ++attachment_index)
            message.attachments.enqueue(deserialize_attachment(attachment_bytes));
        VERIFY(attachment_bytes.is_empty());

        auto const* payload = m_unprocessed_bytes.data() + index + sizeof(MessageHeader) + header.attachment_data_size;
        if (message.bytes.try_append(payload, header.payload_size).is_error()) {
            dbgln("TransportSocketWindows: Failed to allocate message buffer for payload_size {}", header.payload_size);
            should_shutdown = ShouldShutdown::Yes;
            break;
        }
        callback(move(message));
        Checked<size_t> new_index = index;
        new_index += header.payload_size;
        new_index += header.attachment_data_size;
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

ErrorOr<TransportHandle> TransportSocketWindows::release_for_transfer()
{
    auto fd = TRY(m_socket->release_fd());
    return TransportHandle { File::adopt_fd(fd) };
}

}
