/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2024, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
    using namespace Core::System;

    if (handle_offsets.is_empty())
        return {};

    if (m_peer_pid == -1)
        return Error::from_string_literal("Transport is not initialized");

    for (auto offset : handle_offsets) {

        u8* ptr = bytes.data() + offset;
        u8* end = bytes.data() + bytes.size();
        if (ptr + sizeof(HandleType) > end)
            return Error::from_string_literal("Not enough bytes");

        auto type = *(HandleType*)ptr;
        ptr += sizeof(HandleType);
        if (type != SocketHandle && type != FileMappingHandle)
            return Error::from_string_literal("Invalid handle type");

        if (type == SocketHandle) {
            if (ptr + sizeof(WSAPROTOCOL_INFO) > end)
                return Error::from_string_literal("Not enough bytes for socket handle");
            auto* pi = (WSAPROTOCOL_INFO*)ptr;
            SOCKET handle = *(SOCKET*)pi;
            if (WSADuplicateSocket(handle, m_peer_pid, pi))
                return Error::from_windows_error();
        } else {
            if (ptr + sizeof(HANDLE) > end)
                return Error::from_string_literal("Not enough bytes for file mapping handle");
            HANDLE& handle = *(HANDLE*)ptr;
            HANDLE peer_process_handle = OpenProcess(PROCESS_DUP_HANDLE, FALSE, m_peer_pid);
            if (!peer_process_handle)
                return Error::from_windows_error();
            ScopeGuard guard = [&] { CloseHandle(peer_process_handle); };
            if (!DuplicateHandle(GetCurrentProcess(), handle, peer_process_handle, &handle, 0, FALSE, DUPLICATE_SAME_ACCESS))
                return Error::from_windows_error();
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
            if (!error.is_errno() || (error.code() != EAGAIN && error.code() != EWOULDBLOCK))
                return error;

            struct pollfd pollfd = {
                .fd = (SOCKET)Core::System::fd_to_handle(m_socket->fd().value()),
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
        auto maybe_bytes_read = m_socket->read_some({ buffer, sizeof(buffer) });

        if (maybe_bytes_read.is_error()) {
            auto error = maybe_bytes_read.release_error();
            if (error.is_syscall() && error.code() == EAGAIN) {
                break;
            }
            if (error.is_syscall() && error.code() == ECONNRESET) {
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
