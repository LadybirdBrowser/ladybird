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
#include <Windows.h>
#include <direct.h>
#include <io.h>
#include <sys/mman.h>

namespace Core::System {

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
                return Error::from_windows_error(GetLastError());
            int dir_fd = _open_osfhandle((intptr_t)dir_handle, 0);
            if (dir_fd != -1)
                return dir_fd;
        }
        return Error::from_syscall("open"sv, -error);
    }
    return rc;
}

ErrorOr<void> close(int fd)
{
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

    if (SetEndOfFile((HANDLE)_get_osfhandle(fd)) == 0)
        return Error::from_windows_error(GetLastError());
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

/* https://github.com/ncm/selectable-socketpair
Copyright 2007, 2010 by Nathan C. Myers <ncm@cantrip.org>
Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

    Redistributions of source code must retain the above copyright notice, this
    list of conditions and the following disclaimer.

    Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

    The name of the author must not be used to endorse or promote products
    derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
static int dumb_socketpair(SOCKET socks[2], int make_overlapped)
{
    union {
        struct sockaddr_in inaddr;
        struct sockaddr addr;
    } a;
    SOCKET listener;
    int e;
    socklen_t addrlen = sizeof(a.inaddr);
    DWORD flags = (make_overlapped ? WSA_FLAG_OVERLAPPED : 0);
    int reuse = 1;

    if (socks == 0) {
        WSASetLastError(WSAEINVAL);
        return SOCKET_ERROR;
    }
    socks[0] = socks[1] = -1;

    listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET)
        return SOCKET_ERROR;

    memset(&a, 0, sizeof(a));
    a.inaddr.sin_family = AF_INET;
    a.inaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.inaddr.sin_port = 0;

    for (;;) {
        if (::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, (socklen_t)sizeof(reuse)) == -1)
            break;
        if (bind(listener, &a.addr, sizeof(a.inaddr)) == SOCKET_ERROR)
            break;

        memset(&a, 0, sizeof(a));
        if (getsockname(listener, &a.addr, &addrlen) == SOCKET_ERROR)
            break;
        // win32 getsockname may only set the port number, p=0.0005.
        // ( http://msdn.microsoft.com/library/ms738543.aspx ):
        a.inaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.inaddr.sin_family = AF_INET;

        if (::listen(listener, 1) == SOCKET_ERROR)
            break;

        socks[0] = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, flags);
        if (socks[0] == INVALID_SOCKET)
            break;
        if (connect(socks[0], &a.addr, sizeof(a.inaddr)) == SOCKET_ERROR)
            break;

        socks[1] = ::accept(listener, NULL, NULL);
        if (socks[1] == INVALID_SOCKET)
            break;

        closesocket(listener);
        return 0;
    }

    e = WSAGetLastError();
    closesocket(listener);
    closesocket(socks[0]);
    closesocket(socks[1]);
    WSASetLastError(e);
    socks[0] = socks[1] = -1;
    return SOCKET_ERROR;
}

ErrorOr<void> socketpair(int domain, int type, int protocol, int sv[2])
{
    if (domain != AF_LOCAL || type != SOCK_STREAM || protocol != 0)
        return Error::from_string_literal("Unsupported argument value");

    SOCKET socks[2] = {};
    if (dumb_socketpair(socks, true))
        return Error::from_windows_error();

    sv[0] = handle_to_fd(socks[0], SocketHandle);
    sv[1] = handle_to_fd(socks[1], SocketHandle);
    return {};
}

}
