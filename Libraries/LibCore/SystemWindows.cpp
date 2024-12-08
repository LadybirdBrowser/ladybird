/*
 * Copyright (c) 2021-2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021-2022, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Matthias Zimmerman <matthias291999@gmail.com>
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2024, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/ScopeGuard.h>
#include <LibCore/System.h>
#include <direct.h>
#include <sys/mman.h>

#include <AK/Windows.h>

namespace Core::System {

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

int handle_to_fd(HANDLE handle, HandleType type)
{
    return handle_to_fd((intptr_t)handle, type);
}

int handle_to_fd(intptr_t handle, HandleType type)
{
    if (type != SocketHandle && type != FileMappingHandle)
        return _open_osfhandle(handle, 0);

    // Special treatment for socket and file mapping handles because:
    // * _open_osfhandle doesn't support file mapping handles
    // * _close doesn't properly support socket handles (it calls CloseHandle instead of closesocket)
    // Handle value is held in lower 31 bits, and sign bit is set to indicate this is not a regular fd.
    VERIFY((handle >> 31) == 0); // must be 0 ⩽ handle ⩽ 0x7FFFFFFF
    return (1 << 31) | handle;
}

HANDLE fd_to_handle(int fd)
{
    if (fd >= 0)
        return (HANDLE)_get_osfhandle(fd);
    if (fd == -1)
        return INVALID_HANDLE_VALUE;
    return (HANDLE)(intptr_t)(fd & ~(1 << 31));
}

ErrorOr<int> open(StringView path, int options, mode_t mode)
{
    ByteString string_path = path;
    auto sz_path = string_path.characters();
    int rc = _open(sz_path, options | O_BINARY, mode);
    if (rc < 0) {
        int error = errno;
        struct stat st = {};
        if (::stat(sz_path, &st) == 0 && (st.st_mode & S_IFDIR)) {
            HANDLE dir_handle = CreateFile(sz_path, GENERIC_ALL, 0, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
            if (dir_handle == INVALID_HANDLE_VALUE)
                return Error::from_windows_error();
            return handle_to_fd(dir_handle, DirectoryHandle);
        }
        return Error::from_syscall("open"sv, -error);
    }
    return rc;
}

ErrorOr<void> close(int fd)
{
    if (fd < 0) {
        HANDLE handle = fd_to_handle(fd);
        if (handle == INVALID_HANDLE_VALUE)
            return Error::from_string_literal("Invalid file descriptor");
        if (is_socket(fd)) {
            if (closesocket((SOCKET)handle))
                return Error::from_windows_error();
        } else {
            if (!CloseHandle(handle))
                return Error::from_windows_error();
        }
        return {};
    }

    if (_close(fd) < 0)
        return Error::from_syscall("close"sv, -errno);
    return {};
}

ErrorOr<ssize_t> read(int fd, Bytes buffer)
{
    int rc = _read(fd, buffer.data(), buffer.size());
    if (rc < 0)
        return Error::from_syscall("read"sv, -errno);
    return rc;
}

ErrorOr<ssize_t> write(int fd, ReadonlyBytes buffer)
{
    int rc = _write(fd, buffer.data(), buffer.size());
    if (rc < 0)
        return Error::from_syscall("write"sv, -errno);
    return rc;
}

ErrorOr<off_t> lseek(int fd, off_t offset, int whence)
{
    long rc = _lseek(fd, offset, whence);
    if (rc < 0)
        return Error::from_syscall("lseek"sv, -errno);
    return rc;
}

ErrorOr<void> ftruncate(int fd, off_t length)
{
    long position = _tell(fd);
    if (position == -1)
        return Error::from_errno(errno);

    ScopeGuard restore_position { [&] { _lseek(fd, position, SEEK_SET); } };

    auto result = lseek(fd, length, SEEK_SET);
    if (result.is_error())
        return result.release_error();

    if (SetEndOfFile(fd_to_handle(fd)) == 0)
        return Error::from_windows_error();
    return {};
}

ErrorOr<struct stat> fstat(int fd)
{
    struct stat st = {};
    if (::fstat(fd, &st) < 0)
        return Error::from_syscall("fstat"sv, -errno);
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
        return Error::from_syscall("getcwd"sv, -errno);

    ByteString string_cwd(cwd);
    free(cwd);
    return string_cwd;
}

ErrorOr<struct stat> stat(StringView path)
{
    if (path.is_null())
        return Error::from_syscall("stat"sv, -EFAULT);

    struct stat st = {};
    ByteString path_string = path;
    if (::stat(path_string.characters(), &st) < 0)
        return Error::from_syscall("stat"sv, -errno);
    return st;
}

ErrorOr<void> rmdir(StringView path)
{
    if (path.is_null())
        return Error::from_errno(EFAULT);

    ByteString path_string = path;
    if (_rmdir(path_string.characters()) < 0)
        return Error::from_syscall("rmdir"sv, -errno);
    return {};
}

ErrorOr<void> unlink(StringView path)
{
    if (path.is_null())
        return Error::from_errno(EFAULT);

    ByteString path_string = path;
    if (_unlink(path_string.characters()) < 0)
        return Error::from_syscall("unlink"sv, -errno);
    return {};
}

ErrorOr<void> mkdir(StringView path, mode_t)
{
    ByteString str = path;
    if (_mkdir(str.characters()) < 0)
        return Error::from_syscall("mkdir"sv, -errno);
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

ErrorOr<void*> mmap(void* address, size_t size, int protection, int flags, int fd, off_t offset, size_t alignment, StringView)
{
    // custom alignment is not supported
    VERIFY(!alignment);
    void* ptr = ::mmap(address, size, protection, flags, fd, offset);
    if (ptr == MAP_FAILED)
        return Error::from_syscall("mmap"sv, -errno);
    return ptr;
}

ErrorOr<void> munmap(void* address, size_t size)
{
    if (::munmap(address, size) < 0)
        return Error::from_syscall("munmap"sv, -errno);
    return {};
}

int getpid()
{
    return GetCurrentProcessId();
}

ErrorOr<int> dup(int fd)
{
    if (fd < 0) {
        HANDLE handle = fd_to_handle(fd);
        if (handle == INVALID_HANDLE_VALUE)
            return Error::from_string_literal("Invalid file descriptor");

        if (is_socket(fd)) {
            WSAPROTOCOL_INFO pi = {};
            if (WSADuplicateSocket((SOCKET)handle, GetCurrentProcessId(), &pi))
                return Error::from_windows_error();
            SOCKET socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, &pi, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
            if (socket == INVALID_SOCKET)
                return Error::from_windows_error();
            return handle_to_fd(socket, SocketHandle);
        } else {
            if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &handle, 0, FALSE, DUPLICATE_SAME_ACCESS))
                return Error::from_windows_error();
            return handle_to_fd(handle, FileMappingHandle);
        }
    }

    int new_fd = _dup(fd);
    if (new_fd < 0)
        return Error::from_syscall("dup"sv, -errno);
    return new_fd;
}

bool is_socket(int fd)
{
    int val, len = sizeof(val);
    return !::getsockopt((SOCKET)fd_to_handle(fd), SOL_SOCKET, SO_TYPE, (char*)&val, &len);
}

}
