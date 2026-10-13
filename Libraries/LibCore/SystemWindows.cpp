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
#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Checked.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <LibCore/MappedFile.h>
#include <LibCore/Process.h>
#include <LibCore/SocketAddress.h>
#include <LibCore/System.h>
#include <direct.h>

#include <AK/Windows.h>
#include <sddl.h>
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

ErrorOr<void*> reserve_address_space(size_t size, [[maybe_unused]] MemoryTag tag)
{
    void* ptr = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
    if (!ptr)
        return Error::from_windows_error();
    return ptr;
}

ErrorOr<void*> allocate_anonymous_memory(size_t size, [[maybe_unused]] MemoryTag tag)
{
    void* ptr = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!ptr)
        return Error::from_windows_error();
    return ptr;
}

ErrorOr<void> commit_memory(void* address, size_t size, [[maybe_unused]] MemoryTag tag)
{
    if (!VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE))
        return Error::from_windows_error();
    return {};
}

ErrorOr<void> protect_memory_readonly(void* address, size_t size)
{
    if (size == 0)
        return {};
    DWORD previous_protection;
    if (!VirtualProtect(address, size, PAGE_READONLY, &previous_protection))
        return Error::from_windows_error();
    return {};
}

ErrorOr<void> decommit_memory(void* address, size_t size, [[maybe_unused]] MemoryTag tag)
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

ErrorOr<void> map_shared_memory_fixed(void*, size_t, int)
{
    // FIXME: Placing a file-mapping view at a fixed address inside a reservation needs the Windows 10+ placeholder APIs.
    return Error::from_errno(ENOTSUP);
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
    int type = 0;
    int length = sizeof(type);
    return ::getsockopt(static_cast<SOCKET>(handle), SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &length) == 0;
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

ErrorOr<size_t> recv(int sockfd, Bytes buffer, int flags)
{
    auto received = ::recv(sockfd, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), flags);

    if (received == SOCKET_ERROR) {
        auto error = WSAGetLastError();

        return error == WSAEWOULDBLOCK
            ? Error::from_errno(EWOULDBLOCK)
            : Error::from_windows_error(error);
    }

    return received;
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

// A security descriptor that admits only the user this process runs as, where the default would let every user on
// the machine open what it protects for reading.
class CurrentUserOnlySecurityAttributes {
    AK_MAKE_NONCOPYABLE(CurrentUserOnlySecurityAttributes);
    AK_MAKE_NONMOVABLE(CurrentUserOnlySecurityAttributes);

public:
    AK_ALLOC_WITH_KMALLOC;

    static ErrorOr<NonnullOwnPtr<CurrentUserOnlySecurityAttributes>> create()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return Error::from_windows_error();
        ScopeGuard close_token = [&] { CloseHandle(token); };

        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        auto user_buffer = TRY(ByteBuffer::create_zeroed(size));
        if (!GetTokenInformation(token, TokenUser, user_buffer.data(), size, &size))
            return Error::from_windows_error();

        LPSTR user_sid = nullptr;
        if (!ConvertSidToStringSidA(reinterpret_cast<TOKEN_USER*>(user_buffer.data())->User.Sid, &user_sid))
            return Error::from_windows_error();
        ScopeGuard free_sid = [&] { LocalFree(user_sid); };

        auto descriptor_string = ByteString::formatted("D:P(A;;GA;;;{})", user_sid);
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(descriptor_string.characters(), SDDL_REVISION_1, &descriptor, nullptr))
            return Error::from_windows_error();
        return adopt_own(*new CurrentUserOnlySecurityAttributes(descriptor));
    }

    ~CurrentUserOnlySecurityAttributes()
    {
        LocalFree(m_attributes.lpSecurityDescriptor);
    }

    SECURITY_ATTRIBUTES* attributes() { return &m_attributes; }

private:
    explicit CurrentUserOnlySecurityAttributes(PSECURITY_DESCRIPTOR descriptor)
    {
        m_attributes.nLength = sizeof(m_attributes);
        m_attributes.lpSecurityDescriptor = descriptor;
        m_attributes.bInheritHandle = FALSE;
    }

    SECURITY_ATTRIBUTES m_attributes {};
};

static ErrorOr<void> wait_for_pipe_client(HANDLE server)
{
    OVERLAPPED overlapped {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent)
        return Error::from_windows_error();
    ScopeGuard close_event = [&] { CloseHandle(overlapped.hEvent); };

    if (ConnectNamedPipe(server, &overlapped))
        return {};
    auto error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED)
        return {};
    if (error != ERROR_IO_PENDING)
        return Error::from_windows_error(error);
    DWORD ignored = 0;
    if (!GetOverlappedResult(server, &overlapped, &ignored, TRUE))
        return Error::from_windows_error();
    return {};
}

// Two ends of one connection, as socketpair() gives on other systems. A named pipe, rather than a loopback socket,
// keeps what one end wrote readable after that end has closed, however the other end behaves meanwhile. Both ends
// are overlapped, so that a Notifier can watch them, and neither is inherited by a spawned process unless asked to.
ErrorOr<Array<int, 2>> duplex_pipe()
{
    auto security_attributes = TRY(CurrentUserOnlySecurityAttributes::create());
    auto name = ByteString::formatted("\\\\.\\pipe\\ladybird-{}-{:016x}", GetCurrentProcessId(), get_random<u64>());

    constexpr DWORD buffer_size = 1 * MiB;
    HANDLE server = CreateNamedPipeA(name.characters(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, buffer_size, buffer_size, 0, security_attributes->attributes());
    if (server == INVALID_HANDLE_VALUE)
        return Error::from_windows_error();
    ArmedScopeGuard close_server = [&] { CloseHandle(server); };

    HANDLE client = CreateFileA(name.characters(), GENERIC_READ | GENERIC_WRITE, 0, security_attributes->attributes(), OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (client == INVALID_HANDLE_VALUE)
        return Error::from_windows_error();
    ArmedScopeGuard close_client = [&] { CloseHandle(client); };

    TRY(wait_for_pipe_client(server));

    // The name was ours alone to know, but a stranger that raced us to it must not get a channel for it.
    DWORD client_process_id = 0;
    if (!GetNamedPipeClientProcessId(server, &client_process_id))
        return Error::from_windows_error();
    if (client_process_id != GetCurrentProcessId())
        return Error::from_string_literal("Another process connected to our pipe");

    close_server.disarm();
    close_client.disarm();
    return Array<int, 2> { to_fd(server), to_fd(client) };
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

ErrorOr<Array<int, 2>> pipe2(int flags, PipeReadEnd read_end)
{
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = (flags & O_CLOEXEC) ? FALSE : TRUE;

    // Only a named pipe can have an overlapped end, which a Notifier needs to watch it. The name is unpredictable, and
    // the pipe has a single instance, so no other process can take either end of it.
    auto name = ByteString::formatted("\\\\.\\pipe\\ladybird-{}-{:016x}", GetCurrentProcessId(), get_random<u64>());

    DWORD read_end_flags = PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE;
    if (read_end == PipeReadEnd::ReadInternally)
        read_end_flags |= FILE_FLAG_OVERLAPPED;
    constexpr DWORD buffer_size = 64 * KiB;
    HANDLE read_handle = CreateNamedPipeA(name.characters(), read_end_flags, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0, buffer_size, 0, &sa);
    if (read_handle == INVALID_HANDLE_VALUE)
        return Error::from_windows_error();

    HANDLE write_handle = CreateFileA(name.characters(), GENERIC_WRITE, 0, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (write_handle == INVALID_HANDLE_VALUE) {
        auto error = GetLastError();
        CloseHandle(read_handle);
        return Error::from_windows_error(error);
    }

    return Array<int, 2> { to_fd(read_handle), to_fd(write_handle) };
}

ErrorOr<bool> isatty(int handle)
{
    // GetFileType() cannot answer this: it reports FILE_TYPE_CHAR for every character device, including NUL.
    // Only real consoles have a console mode.
    DWORD mode = 0;
    return GetConsoleMode(to_handle(handle), &mode) != 0;
}

ErrorOr<TerminalSize> terminal_size(int fd)
{
    CONSOLE_SCREEN_BUFFER_INFO info {};
    if (!GetConsoleScreenBufferInfo(to_handle(fd), &info))
        return Error::from_windows_error();
    return TerminalSize {
        static_cast<size_t>(info.srWindow.Right - info.srWindow.Left + 1),
        static_cast<size_t>(info.srWindow.Bottom - info.srWindow.Top + 1),
    };
}

ErrorOr<void> enable_ansi_escape_sequence_processing(int fd)
{
    DWORD mode = 0;
    if (!GetConsoleMode(to_handle(fd), &mode))
        return Error::from_windows_error();
    if (!SetConsoleMode(to_handle(fd), mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
        return Error::from_windows_error();
    return {};
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
