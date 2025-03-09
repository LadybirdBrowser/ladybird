/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/File.h>
#include <LibCore/System.h>

#if !defined(AK_OS_WINDOWS)
#    include <unistd.h>
#else
#    include <AK/Windows.h>
#endif

namespace Core {

ErrorOr<NonnullOwnPtr<File>> File::open(StringView filename, OpenMode mode, mode_t permissions)
{
    auto file = TRY(adopt_nonnull_own_or_enomem(new (nothrow) File(mode)));
    TRY(file->open_path(filename, permissions));
    return file;
}

ErrorOr<NonnullOwnPtr<File>> File::adopt_fd(int fd, OpenMode mode, ShouldCloseFileDescriptor should_close_file_descriptor)
{
    if (fd < 0) {
        return Error::from_errno(EBADF);
    }

    if (!has_any_flag(mode, OpenMode::ReadWrite)) {
        dbgln("Core::File::adopt_fd: Attempting to adopt a file with neither Read nor Write specified in mode");
        return Error::from_errno(EINVAL);
    }

    auto file = TRY(adopt_nonnull_own_or_enomem(new (nothrow) File(mode, should_close_file_descriptor)));
    file->m_fd = fd;
    return file;
}

ErrorOr<NonnullOwnPtr<File>> File::standard_input()
{
    return File::adopt_fd(STDIN_FILENO, OpenMode::Read, ShouldCloseFileDescriptor::No);
}
ErrorOr<NonnullOwnPtr<File>> File::standard_output()
{
    return File::adopt_fd(STDOUT_FILENO, OpenMode::Write, ShouldCloseFileDescriptor::No);
}
ErrorOr<NonnullOwnPtr<File>> File::standard_error()
{
    return File::adopt_fd(STDERR_FILENO, OpenMode::Write, ShouldCloseFileDescriptor::No);
}

ErrorOr<NonnullOwnPtr<File>> File::open_file_or_standard_stream(StringView filename, OpenMode mode)
{
    if (!filename.is_empty() && filename != "-"sv)
        return File::open(filename, mode);

    switch (mode) {
    case OpenMode::Read:
        return standard_input();
    case OpenMode::Write:
        return standard_output();
    default:
        VERIFY_NOT_REACHED();
    }
}

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
    if (has_flag(mode, OpenMode::Nonblocking)) {
#if !defined(AK_OS_WINDOWS)
        flags |= O_NONBLOCK;
#else
        dbgln("Core::File::OpenMode::Nonblocking is not implemented");
        VERIFY_NOT_REACHED();
#endif
    }

    // Some open modes, like `ReadWrite` imply the ability to create the file if it doesn't exist.
    // Certain applications may not want this privledge, and for compability reasons, this is
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
        // NOTE: POSIX says that if the fd is not open for reading, the call
        //       will return EBADF. Since we already know whether we can or
        //       can't read the file, let's avoid a syscall.
        return Error::from_errno(EBADF);
    }

    ssize_t nread = TRY(System::read(m_fd, buffer));
    m_last_read_was_eof = nread == 0;
    m_file_offset += nread;
    return buffer.trim(nread);
}

ErrorOr<ByteBuffer> File::read_until_eof(size_t block_size)
{
    // Note: This is used as a heuristic, it's not valid for devices or virtual files.
    auto const potential_file_size = TRY(System::fstat(m_fd)).st_size;

    return read_until_eof_impl(block_size, potential_file_size);
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

bool File::is_eof() const { return m_last_read_was_eof; }
bool File::is_open() const { return m_fd >= 0; }

void File::close()
{
    if (!is_open()) {
        return;
    }

    // NOTE: The closing of the file can be interrupted by a signal, in which
    // case EINTR will be returned by the close syscall. So let's try closing
    // the file until we aren't interrupted by rude signals. :^)
    ErrorOr<void> result;
    do {
        result = System::close(m_fd);
    } while (result.is_error() && result.error().code() == EINTR);

    VERIFY(!result.is_error());
    m_fd = -1;
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

ErrorOr<size_t> File::tell() const
{
    return m_file_offset;
}

ErrorOr<void> File::truncate(size_t length)
{
    if (length > static_cast<size_t>(NumericLimits<off_t>::max()))
        return Error::from_string_literal("Length is larger than the maximum supported length");

    m_file_offset = min(length, m_file_offset);
    return System::ftruncate(m_fd, length);
}

ErrorOr<void> File::set_blocking(bool enabled)
{
    // NOTE: This works fine on Serenity, but some systems out there don't support changing the blocking state of certain POSIX objects (message queues, pipes, etc) after their creation.
    // Therefore, this method shouldn't be used in Lagom.
    // https://github.com/SerenityOS/serenity/pull/18965#discussion_r1207951840
    int value = enabled ? 0 : 1;
    return System::ioctl(fd(), FIONBIO, &value);
}

}
