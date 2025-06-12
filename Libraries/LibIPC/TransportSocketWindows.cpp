/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteReader.h>
#include <LibIPC/HandleType.h>
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
            if (span.size() < sizeof(WSAPROTOCOL_INFO))
                return Error::from_string_literal("Not enough bytes for socket handle");

            // We stashed the bytes of this process's version of the handle at the offset location
            int handle = -1;
            ByteReader::load(span.data(), handle);

            auto* pi = reinterpret_cast<WSAPROTOCOL_INFO*>(span.data());
            if (WSADuplicateSocket(handle, m_peer_pid, pi))
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

ErrorOr<void> TransportSocketWindows::transfer(Bytes bytes_to_write, Vector<size_t> const& handle_offsets)
{
    TRY(duplicate_handles(bytes_to_write, handle_offsets));

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
            VERIFY_NOT_REACHED();
        }

        bytes_to_write = bytes_to_write.slice(maybe_nwritten.value());
    }
    return {};
}

TransportSocketWindows::ReadResult TransportSocketWindows::read_as_much_as_possible_without_blocking(Function<void()> schedule_shutdown)
{
    ReadResult result;

    while (is_open()) {

        u8 buffer[4096];
        auto maybe_bytes_read = m_socket->read_without_waiting({ buffer, sizeof(buffer) });

        if (maybe_bytes_read.is_error()) {
            auto error = maybe_bytes_read.release_error();
            if (error.code() == EWOULDBLOCK)
                break;
            if (error.code() == ECONNRESET) {
                schedule_shutdown();
                break;
            }
            VERIFY_NOT_REACHED();
        }

        auto bytes_read = maybe_bytes_read.release_value();
        if (bytes_read.is_empty()) {
            schedule_shutdown();
            break;
        }

        result.bytes.append(bytes_read.data(), bytes_read.size());
    }

    return result;
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
