/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibCore/File.h>
#include <LibCore/MappedFile.h>
#include <LibCore/System.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace Core {

ErrorOr<NonnullOwnPtr<MappedFile>> MappedFile::map(StringView path, Mode mode)
{
    auto const file_mode = mode == Mode::ReadOnly ? O_RDONLY : O_RDWR;
    auto fd = TRY(Core::System::open(path, file_mode | O_CLOEXEC, 0));
    return map_from_fd_and_close(fd, path, mode);
}

ErrorOr<NonnullOwnPtr<MappedFile>> MappedFile::map_from_file(NonnullOwnPtr<Core::File> stream, StringView path)
{
    return map_from_fd_and_close(stream->leak_fd(Badge<MappedFile> {}), path);
}

ErrorOr<NonnullOwnPtr<MappedFile>> MappedFile::map_from_fd_and_close(int fd, [[maybe_unused]] StringView path, Mode mode)
{
    TRY(Core::System::fcntl(fd, F_SETFD, FD_CLOEXEC));

    ScopeGuard fd_close_guard = [fd] {
        ::close(fd);
    };

    auto stat = TRY(Core::System::fstat(fd));
    auto size = stat.st_size;

    int protection;
    int flags;
    switch (mode) {
    case Mode::ReadOnly:
        protection = PROT_READ;
        flags = MAP_SHARED;
        break;
    case Mode::ReadWrite:
        protection = PROT_READ | PROT_WRITE;
        // Don't map a read-write mapping shared as a precaution.
        flags = MAP_PRIVATE;
        break;
    }

    auto* ptr = TRY(Core::System::mmap(nullptr, size, protection, flags, fd, 0, 0, path));

    return adopt_own(*new MappedFile(ptr, size, mode));
}

MappedFile::MappedFile(void* ptr, size_t size, Mode mode)
    : FixedMemoryStream(Bytes { ptr, size }, mode)
    , m_data(ptr)
    , m_size(size)
{
}

MappedFile::~MappedFile()
{
    auto res = Core::System::munmap(m_data, m_size);
    if (res.is_error())
        dbgln("Failed to unmap MappedFile (@ {:p}): {}", m_data, res.error());
}

}
