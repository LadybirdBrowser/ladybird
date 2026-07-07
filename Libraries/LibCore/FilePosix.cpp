/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

}
