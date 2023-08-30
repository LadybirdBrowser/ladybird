/*
 * Copyright (c) 2022, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Directory.h"
#include "DirIterator.h"
#include "System.h"
#if !defined(AK_OS_WINDOWS)
#    include <dirent.h>
#else
#    include <io.h>
#    include <winbase.h>
#    include <windows.h>
#    include <winternl.h>
#endif

namespace Core {

// We assume that the fd is a valid directory.
#if !defined(AK_OS_WINDOWS)
Directory::Directory(int fd, LexicalPath path)
    : m_path(move(path))
    , m_directory_fd(fd)
{
}

Directory::Directory(Directory&& other)
    : m_path(move(other.m_path))
    , m_directory_fd(other.m_directory_fd)
{
    other.m_directory_fd = -1;
}
#else
Directory::Directory(HANDLE handle, LexicalPath path)
    : m_path(move(path))
    , m_directory_handle(handle)
{
}

Directory::Directory(Directory&& other)
    : m_path(move(other.m_path))
    , m_directory_handle(other.m_directory_handle)
{
    other.m_directory_handle = INVALID_HANDLE_VALUE;
}
#endif

Directory::~Directory()
{
#if !defined(AK_OS_WINDOWS)
    if (m_directory_fd != -1)
        MUST(System::close(m_directory_fd));
#else
    if (m_directory_handle != INVALID_HANDLE_VALUE)
        CloseHandle(m_directory_handle);
#endif
}

#if !defined(AK_OS_WINDOWS)
ErrorOr<void> Directory::chown(uid_t uid, gid_t gid)
{
    if (m_directory_fd == -1)
        return Error::from_syscall("fchown"sv, -EBADF);
    TRY(Core::System::fchown(m_directory_fd, uid, gid));
    return {};
}
#endif

#if !defined(AK_OS_WINDOWS)
ErrorOr<bool> Directory::is_valid_directory(int fd)
{
    auto stat = TRY(System::fstat(fd));
    return stat.st_mode & S_IFDIR;
}

ErrorOr<Directory> Directory::adopt_fd(int fd, LexicalPath path)
{
    // This will also fail if the fd is invalid in the first place.
    if (!TRY(Directory::is_valid_directory(fd)))
        return Error::from_errno(ENOTDIR);
    return Directory { fd, move(path) };
}
#else
ErrorOr<bool> Directory::is_valid_directory(HANDLE handle)
{
    // Stat the file to see if it's a directory.
    FILE_BASIC_INFO basic_info;
    if (GetFileInformationByHandleEx(handle, FileBasicInfo, &basic_info, sizeof(basic_info)) == 0)
        return Error::from_errno(ENOTDIR);
    return basic_info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY;
}

ErrorOr<Directory> Directory::adopt_handle(HANDLE handle, LexicalPath path)
{
    // This will also fail if the fd is invalid in the first place.
    if (!TRY(Directory::is_valid_directory(handle)))
        return Error::from_errno(ENOTDIR);
    return Directory { handle, move(path) };
}
#endif

ErrorOr<Directory> Directory::create(ByteString path, CreateDirectories create_directories, mode_t creation_mode)
{
    return create(LexicalPath { move(path) }, create_directories, creation_mode);
}

ErrorOr<Directory> Directory::create(LexicalPath path, CreateDirectories create_directories, mode_t creation_mode)
{
    if (create_directories == CreateDirectories::Yes)
        TRY(ensure_directory(path, creation_mode));
        // FIXME: doesn't work on Linux probably
#if !defined(AK_OS_WINDOWS)
    auto fd = TRY(System::open(path.string(), O_CLOEXEC));
    return adopt_fd(fd, move(path));
#else
    auto* handle = CreateFile(path.string().characters(), FILE_LIST_DIRECTORY | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    return adopt_handle(handle, move(path));
#endif
}

ErrorOr<void> Directory::ensure_directory(LexicalPath const& path, mode_t creation_mode)
{
    if (path.basename() == "/" || path.basename() == ".")
        return {};

    TRY(ensure_directory(path.parent(), creation_mode));

    auto return_value = System::mkdir(path.string(), creation_mode);
    // We don't care if the directory already exists.
    if (return_value.is_error() && return_value.error().code() != EEXIST)
        return return_value;

    return {};
}

ErrorOr<NonnullOwnPtr<File>> Directory::open(StringView filename, File::OpenMode mode) const
{
#if !defined(AK_OS_WINDOWS)
    auto fd = TRY(System::openat(m_directory_fd, filename, File::open_mode_to_options(mode)));
    return File::adopt_fd(fd, mode);
#else
    (void)mode;
    dbgln("Directory::open({}) not implemented on Windows", filename);
    VERIFY_NOT_REACHED();
#endif
}

ErrorOr<struct stat> Directory::stat(StringView filename, int flags) const
{
#if !defined(AK_OS_WINDOWS)
    return System::fstatat(m_directory_fd, filename, flags);
#else
    (void)flags;
    dbgln("Directory::stat({}) not implemented on Windows", filename);
    VERIFY_NOT_REACHED();
#endif
}

ErrorOr<struct stat> Directory::stat() const
{
#if !defined(AK_OS_WINDOWS)
    return System::fstat(m_directory_fd);
#else
    return System::fstat(_open_osfhandle((intptr_t)m_directory_handle, 0));
#endif
}

ErrorOr<void> Directory::for_each_entry(DirIterator::Flags flags, Core::Directory::ForEachEntryCallback callback)
{
    DirIterator iterator { path().string(), flags };
    if (iterator.has_error())
        return iterator.error();

    while (iterator.has_next()) {
        if (iterator.has_error())
            return iterator.error();

        auto entry = iterator.next();
        if (!entry.has_value())
            break;

        auto decision = TRY(callback(entry.value(), *this));
        if (decision == IterationDecision::Break)
            break;
    }

    return {};
}

ErrorOr<void> Directory::for_each_entry(AK::StringView path, DirIterator::Flags flags, Core::Directory::ForEachEntryCallback callback)
{
    auto directory = TRY(Directory::create(path, CreateDirectories::No));
    return directory.for_each_entry(flags, move(callback));
}

}
