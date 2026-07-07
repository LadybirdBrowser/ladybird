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

ErrorOr<ByteBuffer> File::read_until_eof(size_t block_size)
{
    // Note: This is used as a heuristic, it's not valid for devices or virtual files.
    auto const potential_file_size = TRY(stat()).st_size;

    return read_until_eof_impl(block_size, potential_file_size);
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

ErrorOr<size_t> File::tell() const
{
    return m_file_offset;
}

}
