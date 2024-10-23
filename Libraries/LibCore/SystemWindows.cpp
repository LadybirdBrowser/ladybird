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

namespace Core::System {

ErrorOr<int> open(StringView path, int options, mode_t mode)
{
    ByteString string_path = path;
    auto sz_path = string_path.characters();
    int rc = _open(sz_path, options, mode);
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

}
