/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <LibCore/File.h>
#include <LibCore/System.h>

#include <unistd.h>

namespace Core {

int File::open_mode_to_options(OpenMode mode)
{
    int flags = 0;
    if (has_flag(mode, OpenMode::ReadWrite)) {
        flags |= O_RDWR | O_CREAT;
    } else if (has_flag(mode, OpenMode::Read)) {
        flags |= O_RDONLY;
    } else if (has_flag(mode, OpenMode::Write)) {
        flags |= O_WRONLY | O_CREAT;
        bool should_truncate = !has_any_flag(mode, OpenMode::Append | OpenMode::MustBeNew);
        if (should_truncate)
            flags |= O_TRUNC;
    }

    if (has_flag(mode, OpenMode::Append))
        flags |= O_APPEND;
    if (has_flag(mode, OpenMode::Truncate))
        flags |= O_TRUNC;
    if (has_flag(mode, OpenMode::MustBeNew))
        flags |= O_EXCL;
    if (!has_flag(mode, OpenMode::KeepOnExec))
        flags |= O_CLOEXEC;
    if (has_flag(mode, OpenMode::Nonblocking))
        flags |= O_NONBLOCK;
    if (has_flag(mode, OpenMode::NoFollow))
        flags |= O_NOFOLLOW;

    // Some open modes, like `ReadWrite` imply the ability to create the file if it doesn't exist.
    // Certain applications may not want this privilege, and for compatibility reasons, this is
    // the easiest way to add this option.
    if (has_flag(mode, OpenMode::DontCreate))
        flags &= ~O_CREAT;

    return flags;
}

ErrorOr<void> File::open_path(StringView filename, mode_t permissions)
{
    VERIFY(m_fd == -1);
    auto flags = open_mode_to_options(m_mode);

    m_fd = TRY(System::open(filename, flags, permissions));
    return {};
}

ErrorOr<Bytes> File::read_some(Bytes buffer)
{
    if (!has_flag(m_mode, OpenMode::Read)) {
        return Error::from_errno(EBADF);
    }

    auto nread = TRY(System::read(m_fd, buffer));
    m_last_read_was_eof = nread == 0;
    m_file_offset += nread;
    return buffer.trim(nread);
}

ErrorOr<size_t> File::write_some(ReadonlyBytes buffer)
{
    if (!has_flag(m_mode, OpenMode::Write)) {
        // NOTE: Same deal as Read.
        return Error::from_errno(EBADF);
    }

    auto nwritten = TRY(System::write(m_fd, buffer));
    m_file_offset += nwritten;
    return nwritten;
}

ErrorOr<size_t> File::seek(i64 offset, SeekMode mode)
{
    int syscall_mode;
    switch (mode) {
    case SeekMode::SetPosition:
        syscall_mode = SEEK_SET;
        break;
    case SeekMode::FromCurrentPosition:
        syscall_mode = SEEK_CUR;
        break;
    case SeekMode::FromEndPosition:
        syscall_mode = SEEK_END;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    size_t seek_result = TRY(System::lseek(m_fd, offset, syscall_mode));
    m_file_offset = seek_result;
    m_last_read_was_eof = false;
    return seek_result;
}

ErrorOr<void> File::truncate(size_t length)
{
    if (length > static_cast<size_t>(NumericLimits<off_t>::max()))
        return Error::from_string_literal("Length is larger than the maximum supported length");

    m_file_offset = min(length, m_file_offset);
    return System::ftruncate(m_fd, length);
}

ErrorOr<struct stat> File::stat() const
{
    return System::fstat(m_fd);
}

ErrorOr<struct stat> File::stat(StringView path)
{
    return System::stat(path);
}

ErrorOr<struct stat> File::fstat(int fd)
{
    return System::fstat(fd);
}

ErrorOr<void> File::set_blocking(bool enabled)
{
    return System::set_socket_blocking(fd(), enabled);
}

ErrorOr<NonnullOwnPtr<File>> File::open_regular_file_or_directory(StringView canonical_path)
{
    // Intermediate directories need search permission, not read permission.
#if defined(O_PATH)
    static constexpr int directory_options = O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
#elif defined(O_SEARCH)
    static constexpr int directory_options = O_SEARCH | O_NOFOLLOW | O_CLOEXEC;
#else
    static constexpr int directory_options = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
#endif
    static constexpr int file_options = O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC;

    auto is_regular_file_or_directory = [](struct stat const& stat) {
        return S_ISREG(stat.st_mode) || S_ISDIR(stat.st_mode);
    };

    LexicalPath lexical_path { canonical_path };
    if (!lexical_path.is_absolute())
        return Error::from_errno(EINVAL);
    auto const& components = lexical_path.parts_view();

    int fd = -1;
    if (components.is_empty()) {
        fd = TRY(System::open("/"sv, file_options));
    } else {
        int directory_fd = TRY(System::open("/"sv, directory_options));
        ScopeGuard close_directory = [&] { (void)System::close(directory_fd); };
        for (size_t i = 0; i + 1 < components.size(); ++i) {
            auto next_directory_fd = TRY(System::openat(directory_fd, components[i], directory_options));
            (void)System::close(directory_fd);
            directory_fd = next_directory_fd;
        }

        if (!is_regular_file_or_directory(TRY(System::fstatat(directory_fd, components.last(), AT_SYMLINK_NOFOLLOW))))
            return Error::from_errno(EACCES);
        fd = TRY(System::openat(directory_fd, components.last(), file_options));
    }

    auto file_or_error = adopt_fd(fd, OpenMode::Read);
    if (file_or_error.is_error()) {
        (void)System::close(fd);
        return file_or_error.release_error();
    }
    auto file = file_or_error.release_value();
    if (!is_regular_file_or_directory(TRY(file->stat())))
        return Error::from_errno(EACCES);
    TRY(file->set_blocking(true));
    return file;
}

}
