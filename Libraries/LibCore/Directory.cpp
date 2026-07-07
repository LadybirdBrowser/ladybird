/*
 * Copyright (c) 2022, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Directory.h>
#include <LibCore/System.h>

#ifdef AK_OS_WINDOWS
#    include <AK/Windows.h>
#endif

namespace Core {

// We assume that the fd is a valid directory.
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

Directory::~Directory()
{
    if (m_directory_fd != -1)
        MUST(System::close(m_directory_fd));
}

#ifndef AK_OS_WINDOWS
ErrorOr<void> Directory::chown(uid_t uid, gid_t gid)
{
    if (m_directory_fd == -1)
        return Error::from_syscall("fchown"sv, EBADF);
    TRY(Core::System::fchown(m_directory_fd, uid, gid));
    return {};
}
#endif

ErrorOr<bool> Directory::is_valid_directory(int fd)
{
    auto stat = TRY(File::fstat(fd));
    return stat.st_mode & S_IFDIR;
}

ErrorOr<Directory> Directory::adopt_fd(int fd, LexicalPath path)
{
    // This will also fail if the fd is invalid in the first place.
    if (!TRY(Directory::is_valid_directory(fd)))
        return Error::from_errno(ENOTDIR);
    return Directory { fd, move(path) };
}

ErrorOr<Directory> Directory::create(ByteString path, CreateDirectories create_directories, mode_t creation_mode)
{
    return create(LexicalPath { move(path) }, create_directories, creation_mode);
}

ErrorOr<Directory> Directory::create(LexicalPath path, CreateDirectories create_directories, mode_t creation_mode)
{
    if (create_directories == CreateDirectories::Yes)
        TRY(ensure_directory(path, creation_mode));
    // Validate before leaking the fd out of the File, so that failure (e.g. ENOTDIR
    // for an existing regular file) doesn't leak it.
    auto file = TRY(File::open(path.string(), File::OpenMode::Read));
    if (!TRY(is_valid_directory(file->fd())))
        return Error::from_errno(ENOTDIR);
    return Directory { file->leak_fd(), move(path) };
}

ErrorOr<void> Directory::ensure_directory(LexicalPath const& path, mode_t creation_mode)
{
    if (path.is_root() || path.string() == ".")
        return {};

    TRY(ensure_directory(path.parent(), creation_mode));

    // We don't care if the directory already exists.
#ifdef AK_OS_WINDOWS
    // NOTE: Windows directories have no POSIX permission bits; creation_mode is ignored.
    auto wide_path = TRY(to_wide_string(path.string()));
    if (!CreateDirectoryW(wide_path.data(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return Error::from_windows_error();
#else
    auto return_value = System::mkdir(path.string(), creation_mode);
    if (return_value.is_error() && return_value.error().code() != EEXIST)
        return return_value;
#endif
    return {};
}

#ifdef AK_OS_WINDOWS
ErrorOr<NonnullOwnPtr<File>> Directory::open(StringView filename, File::OpenMode mode) const
{
    // Windows has no fd-relative file APIs, so open by joined path instead.
    return File::open(LexicalPath::join(m_path.string(), filename).string(), mode);
}

ErrorOr<struct stat> Directory::stat(StringView filename) const
{
    return File::stat(LexicalPath::join(m_path.string(), filename).string());
}
#else
ErrorOr<NonnullOwnPtr<File>> Directory::open(StringView filename, File::OpenMode mode) const
{
    // NOTE: openat()'s creation mode defaulted to 0, which made files created through
    //       Directory::open() unreadable and unwritable; use the same default as File::open().
    auto fd = TRY(System::openat(m_directory_fd, filename, File::open_mode_to_options(mode), 0644));
    return File::adopt_fd(fd, mode);
}

ErrorOr<struct stat> Directory::stat(StringView filename) const
{
    return System::fstatat(m_directory_fd, filename, 0);
}
#endif

ErrorOr<struct stat> Directory::stat() const
{
    return File::fstat(m_directory_fd);
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
