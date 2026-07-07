/*
 * Copyright (c) 2021-2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021-2022, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Matthias Zimmerman <matthias291999@gmail.com>
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2024-2025, stasoid <stasoid@yahoo.com>
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/ByteString.h>
#include <AK/Checked.h>
#include <LibCore/MappedFile.h>
#include <LibCore/Process.h>
#include <LibCore/SocketAddress.h>
#include <LibCore/System.h>
#include <direct.h>

#include <AK/Windows.h>
#include <ws2tcpip.h>

namespace Core::System {

int windows_socketpair(SOCKET socks[2], int make_overlapped);

ErrorOr<void> close(int handle)
{
    if (is_socket(handle)) {
        if (closesocket(handle))
            return Error::from_windows_error();
    } else {
        if (!CloseHandle(to_handle(handle)))
            return Error::from_windows_error();
    }
    return {};
}

ErrorOr<void> set_socket_blocking(int socket, bool enabled)
{
    u_long value = enabled ? 0 : 1;
    if (::ioctlsocket(socket, FIONBIO, &value) == SOCKET_ERROR)
        return Error::from_windows_error();
    return {};
}

ErrorOr<ByteString> getcwd()
{
    auto* cwd = _getcwd(nullptr, 0);
    if (!cwd)
        return Error::from_syscall("getcwd"sv, errno);

    ByteString string_cwd(cwd);
    free(cwd);
    return string_cwd;
}

ErrorOr<void> chdir(StringView path)
{
    ByteString path_string = path;
    if (::_chdir(path_string.characters()) < 0)
        return Error::from_syscall("chdir"sv, errno);
    return {};
}

ErrorOr<void*> reserve_address_space(size_t size)
{
    void* ptr = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
    if (!ptr)
        return Error::from_windows_error();
    return ptr;
}

ErrorOr<void> commit_memory(void* address, size_t size)
{
    if (!VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE))
        return Error::from_windows_error();
    return {};
}

ErrorOr<void> decommit_memory(void* address, size_t size)
{
    if (size == 0)
        return {};
    if (!VirtualFree(address, size, MEM_DECOMMIT))
        return Error::from_windows_error();
    return {};
}

ErrorOr<void> release_address_space(void* address, size_t size)
{
    // VirtualFree with MEM_RELEASE requires size == 0 and frees the entire reservation.
    (void)size;
    if (!VirtualFree(address, 0, MEM_RELEASE))
        return Error::from_windows_error();
    return {};
}

int getpid()
{
    return GetCurrentProcessId();
}

ErrorOr<int> dup(int handle)
{
    if (handle < 0) {
        return Error::from_windows_error(ERROR_INVALID_HANDLE);
    }
    if (is_socket(handle)) {
        WSAPROTOCOL_INFOW pi = {};
        if (WSADuplicateSocketW(handle, GetCurrentProcessId(), &pi))
            return Error::from_windows_error();
        SOCKET socket = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, &pi, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
        if (socket == INVALID_SOCKET)
            return Error::from_windows_error();
        return socket;
    } else {
        HANDLE new_handle = 0;
        if (!DuplicateHandle(GetCurrentProcess(), to_handle(handle), GetCurrentProcess(), &new_handle, 0, FALSE, DUPLICATE_SAME_ACCESS))
            return Error::from_windows_error();
        return to_fd(new_handle);
    }
}

bool is_socket(int handle)
{
    // FILE_TYPE_PIPE is returned for sockets and pipes. We don't use Windows pipes.
    return GetFileType(to_handle(handle)) == FILE_TYPE_PIPE;
}

ErrorOr<void> bind(int sockfd, struct sockaddr const* name, socklen_t name_size)
{
    if (::bind(sockfd, name, name_size) == SOCKET_ERROR)
        return Error::from_windows_error();
    return {};
}

ErrorOr<void> listen(int sockfd, int backlog)
{
    if (::listen(sockfd, backlog) == SOCKET_ERROR)
        return Error::from_windows_error();
    return {};
}

ErrorOr<int> accept(int sockfd, struct sockaddr* addr, socklen_t* addr_size)
{
    auto fd = ::accept(sockfd, addr, addr_size);
    if (fd == INVALID_SOCKET)
        return Error::from_windows_error();
    return fd;
}

ErrorOr<void> connect(int sockfd, struct sockaddr const* address, socklen_t address_length)
{
    if (::connect(sockfd, address, address_length) == SOCKET_ERROR)
        return Error::from_windows_error();
    return {};
}

ErrorOr<size_t> send(int sockfd, ReadonlyBytes data, int flags)
{
    auto sent = ::send(sockfd, reinterpret_cast<char const*>(data.data()), static_cast<int>(data.size()), flags);

    if (sent == SOCKET_ERROR) {
        auto error = WSAGetLastError();

        return error == WSAEWOULDBLOCK
            ? Error::from_errno(EWOULDBLOCK)
            : Error::from_windows_error(error);
    }

    return sent;
}

ErrorOr<size_t> sendto(int sockfd, ReadonlyBytes data, int flags, struct sockaddr const* destination, socklen_t destination_length)
{
    auto sent = ::sendto(sockfd, reinterpret_cast<char const*>(data.data()), static_cast<int>(data.size()), flags, destination, destination_length);
    if (sent == SOCKET_ERROR)
        return Error::from_windows_error();
    return sent;
}

ErrorOr<size_t> recvfrom(int sockfd, Bytes buffer, int flags, struct sockaddr* address, socklen_t* address_length)
{
    auto received = ::recvfrom(sockfd, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), flags, address, address_length);
    if (received == SOCKET_ERROR)
        return Error::from_windows_error();
    return received;
}

ErrorOr<void> getsockname(int sockfd, struct sockaddr* name, socklen_t* name_size)
{
    if (::getsockname(sockfd, name, name_size) == SOCKET_ERROR)
        return Error::from_windows_error();
    return {};
}

ErrorOr<void> setsockopt(int sockfd, int level, int option, void const* value, socklen_t value_size)
{
    if (::setsockopt(sockfd, level, option, static_cast<char const*>(value), value_size) == SOCKET_ERROR)
        return Error::from_windows_error();
    return {};
}

ErrorOr<void> socketpair(int domain, int type, int protocol, int sv[2])
{
    if (domain != AF_LOCAL || type != SOCK_STREAM || protocol != 0)
        return Error::from_string_literal("Unsupported argument value");

    SOCKET socks[2] = {};
    if (windows_socketpair(socks, true))
        return Error::from_windows_error();

    sv[0] = socks[0];
    sv[1] = socks[1];
    return {};
}

ErrorOr<void> sleep_ms(u32 milliseconds)
{
    Sleep(milliseconds);
    return {};
}

unsigned hardware_concurrency()
{
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    // number of logical processors in the current group (max 64)
    return si.dwNumberOfProcessors;
}

u64 physical_memory_bytes()
{
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof ms;
    GlobalMemoryStatusEx(&ms);
    return ms.ullTotalPhys;
}

ErrorOr<ByteString> current_executable_path()
{
    return TRY(Process::get_name()).to_byte_string();
}

ErrorOr<void> set_close_on_exec(int handle, bool enabled)
{
    if (!SetHandleInformation(to_handle(handle), HANDLE_FLAG_INHERIT, enabled ? 0 : HANDLE_FLAG_INHERIT))
        return Error::from_windows_error();
    return {};
}

ErrorOr<Array<int, 2>> pipe2(int flags)
{
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = (flags & O_CLOEXEC) ? FALSE : TRUE;

    HANDLE read_handle = nullptr;
    HANDLE write_handle = nullptr;
    if (!CreatePipe(&read_handle, &write_handle, &sa, 0))
        return Error::from_windows_error();

    return Array<int, 2> { to_fd(read_handle), to_fd(write_handle) };
}

ErrorOr<bool> isatty(int handle)
{
    return GetFileType(to_handle(handle)) == FILE_TYPE_CHAR;
}

ErrorOr<int> socket(int domain, int type, int protocol)
{
    auto socket = ::socket(domain, type, protocol);
    if (socket == INVALID_SOCKET)
        return Error::from_windows_error();
    return socket;
}

ErrorOr<AddressInfoVector> getaddrinfo(char const* nodename, char const* servname, struct addrinfo const& hints)
{
    struct addrinfo* results = nullptr;

    int rc = ::getaddrinfo(nodename, servname, &hints, &results);
    if (rc != 0)
        return Error::from_windows_error(rc);

    Vector<struct addrinfo> addresses;

    for (auto* result = results; result != nullptr; result = result->ai_next)
        TRY(addresses.try_append(*result));

    return AddressInfoVector { move(addresses), results };
}

ErrorOr<size_t> transfer_file_through_socket(int source_fd, int target_fd, size_t source_offset, size_t source_length)
{
    // FIXME: We could use TransmitFile (https://learn.microsoft.com/en-us/windows/win32/api/mswsock/nf-mswsock-transmitfile)
    //        here. But in order to transmit a subset of the file, we have to use overlapped IO.

    if (!AK::is_within_range<off_t>(source_offset))
        return Error::from_errno(EOVERFLOW);

    auto mapped_file = TRY(MappedFile::map_from_fd_range_and_close(TRY(dup(source_fd)), {}, static_cast<off_t>(source_offset), source_length));
    return send(target_fd, mapped_file->bytes(), 0);
}

}
