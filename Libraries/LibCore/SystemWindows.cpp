/*
 * Copyright (c) 2021-2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021-2022, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Matthias Zimmerman <matthias291999@gmail.com>
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2024-2025, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/ScopeGuard.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <direct.h>
#include <sys/mman.h>

#include <AK/Windows.h>

namespace Core::System {

int windows_socketpair(SOCKET socks[2], int make_overlapped);

static void invalid_parameter_handler(wchar_t const*, wchar_t const*, wchar_t const*, unsigned int, uintptr_t)
{
}

static int init_crt_and_wsa()
{
    WSADATA wsa;
    WORD version = MAKEWORD(2, 2);
    int rc = WSAStartup(version, &wsa);
    VERIFY(!rc && wsa.wVersion == version);

    // Make _get_osfhandle return -1 instead of crashing on invalid fd in release (debug still __debugbreak's)
    _set_invalid_parameter_handler(invalid_parameter_handler);
    return 0;
}

static auto dummy = init_crt_and_wsa();

ErrorOr<int> open(StringView path, int options, mode_t mode)
{
    ByteString str = path;
    int fd = _open(str.characters(), options | O_BINARY | _O_OBTAIN_DIR, mode);
    if (fd < 0)
        return Error::from_syscall("open"sv, errno);
    ScopeGuard guard = [&] { _close(fd); };
    return dup(_get_osfhandle(fd));
}

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

ErrorOr<ssize_t> read(int handle, Bytes buffer)
{
    DWORD n_read = 0;
    if (!ReadFile(to_handle(handle), buffer.data(), buffer.size(), &n_read, NULL))
        return Error::from_windows_error();
    return n_read;
}

ErrorOr<ssize_t> write(int handle, ReadonlyBytes buffer)
{
    DWORD n_written = 0;
    if (!WriteFile(to_handle(handle), buffer.data(), buffer.size(), &n_written, NULL))
        return Error::from_windows_error();
    return n_written;
}

ErrorOr<off_t> lseek(int handle, off_t offset, int origin)
{
    static_assert(FILE_BEGIN == SEEK_SET && FILE_CURRENT == SEEK_CUR && FILE_END == SEEK_END, "SetFilePointerEx origin values are incompatible with lseek");
    LARGE_INTEGER new_pointer = {};
    if (!SetFilePointerEx(to_handle(handle), { .QuadPart = offset }, &new_pointer, origin))
        return Error::from_windows_error();
    return new_pointer.QuadPart;
}

ErrorOr<void> ftruncate(int handle, off_t length)
{
    auto position = TRY(lseek(handle, 0, SEEK_CUR));
    ScopeGuard restore_position = [&] { MUST(lseek(handle, position, SEEK_SET)); };

    TRY(lseek(handle, length, SEEK_SET));

    if (!SetEndOfFile(to_handle(handle)))
        return Error::from_windows_error();
    return {};
}

ErrorOr<struct stat> fstat(int handle)
{
    struct stat st = {};
    int fd = _open_osfhandle(TRY(dup(handle)), 0);
    ScopeGuard guard = [&] { _close(fd); };
    if (::fstat(fd, &st) < 0)
        return Error::from_syscall("fstat"sv, errno);
    return st;
}

ErrorOr<void> ioctl(int, unsigned, ...)
{
    dbgln("Core::System::ioctl() is not implemented");
    VERIFY_NOT_REACHED();
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
    if (path.is_null())
        return Error::from_errno(EFAULT);

    ByteString path_string = path;
    if (::_chdir(path_string.characters()) < 0)
        return Error::from_syscall("chdir"sv, errno);
    return {};
}

ErrorOr<struct stat> stat(StringView path)
{
    if (path.is_null())
        return Error::from_syscall("stat"sv, EFAULT);

    struct stat st = {};
    ByteString path_string = path;
    if (::stat(path_string.characters(), &st) < 0)
        return Error::from_syscall("stat"sv, errno);
    return st;
}

ErrorOr<void> rmdir(StringView path)
{
    if (path.is_null())
        return Error::from_errno(EFAULT);

    ByteString path_string = path;
    if (_rmdir(path_string.characters()) < 0)
        return Error::from_syscall("rmdir"sv, errno);
    return {};
}

ErrorOr<void> unlink(StringView path)
{
    if (path.is_null())
        return Error::from_errno(EFAULT);

    ByteString path_string = path;
    if (_unlink(path_string.characters()) < 0)
        return Error::from_syscall("unlink"sv, errno);
    return {};
}

ErrorOr<void> mkdir(StringView path, mode_t)
{
    ByteString str = path;
    if (_mkdir(str.characters()) < 0)
        return Error::from_syscall("mkdir"sv, errno);
    return {};
}

ErrorOr<int> openat(int, StringView, int, mode_t)
{
    dbgln("Core::System::openat() is not implemented");
    VERIFY_NOT_REACHED();
}

ErrorOr<struct stat> fstatat(int, StringView, int)
{
    dbgln("Core::System::fstatat() is not implemented");
    VERIFY_NOT_REACHED();
}

ErrorOr<void*> mmap(void* address, size_t size, int protection, int flags, int file_handle, off_t offset, size_t alignment, StringView)
{
    // custom alignment is not supported
    VERIFY(!alignment);
    int fd = _open_osfhandle(TRY(dup(file_handle)), 0);
    ScopeGuard guard = [&] { _close(fd); };
    void* ptr = ::mmap(address, size, protection, flags, fd, offset);
    if (ptr == MAP_FAILED)
        return Error::from_syscall("mmap"sv, errno);
    return ptr;
}

ErrorOr<void> munmap(void* address, size_t size)
{
    if (::munmap(address, size) < 0)
        return Error::from_syscall("munmap"sv, errno);
    return {};
}

int getpid()
{
    return GetCurrentProcessId();
}

ErrorOr<int> dup(int handle)
{
    if (is_socket(handle)) {
        WSAPROTOCOL_INFO pi = {};
        if (WSADuplicateSocket(handle, GetCurrentProcessId(), &pi))
            return Error::from_windows_error();
        SOCKET socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, &pi, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
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

ErrorOr<bool> isatty(int handle)
{
    return GetFileType(to_handle(handle)) == FILE_TYPE_CHAR;
}

}
