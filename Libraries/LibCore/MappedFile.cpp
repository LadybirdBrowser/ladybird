/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/ScopeGuard.h>
#include <LibCore/File.h>
#include <LibCore/MappedFile.h>
#include <LibCore/System.h>
#include <sys/mman.h>

namespace Core {

ErrorOr<NonnullOwnPtr<MappedFile>> MappedFile::map(StringView path, Mode mode)
{
    auto const file_mode = mode == Mode::ReadOnly ? O_RDONLY : O_RDWR;
    auto fd = TRY(Core::System::open(path, file_mode | O_CLOEXEC, 0));
    return map_from_fd_and_close(fd, path, mode);
}

ErrorOr<NonnullOwnPtr<MappedFile>> MappedFile::map_from_file(NonnullOwnPtr<Core::File> stream, StringView path)
{
    return map_from_fd_and_close(stream->leak_fd(), path);
}

ErrorOr<NonnullOwnPtr<MappedFile>> MappedFile::map_from_fd_and_close(int fd, [[maybe_unused]] StringView path, Mode mode)
{
    ArmedScopeGuard fd_close_guard = [fd] {
        (void)System::close(fd);
    };

    auto stat = TRY(Core::System::fstat(fd));
    if (stat.st_size < 0)
        return Error::from_errno(EINVAL);

    fd_close_guard.disarm();
    return map_from_fd_range_and_close(fd, path, 0, static_cast<size_t>(stat.st_size), mode);
}

ErrorOr<NonnullOwnPtr<MappedFile>> MappedFile::map_from_fd_range_and_close(int fd, [[maybe_unused]] StringView path, off_t offset, size_t size, Mode mode)
{
    ScopeGuard fd_close_guard = [fd] {
        (void)System::close(fd);
    };

    if (offset < 0 || !AK::is_within_range<size_t>(offset))
        return Error::from_errno(EINVAL);

    auto stat = TRY(Core::System::fstat(fd));
    if (stat.st_size < 0 || !AK::is_within_range<size_t>(stat.st_size))
        return Error::from_errno(EINVAL);

    auto file_size = static_cast<size_t>(stat.st_size);
    auto requested_offset = static_cast<size_t>(offset);
    if (requested_offset > file_size || size > file_size - requested_offset)
        return Error::from_errno(EINVAL);

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

    if (size == 0)
        return adopt_own(*new MappedFile(nullptr, 0, nullptr, 0, mode));

    auto page_aligned_offset = align_down_to(requested_offset, PAGE_SIZE);
    auto offset_in_mapping = requested_offset - page_aligned_offset;

    Checked<size_t> mapping_size = offset_in_mapping;
    mapping_size += size;
    if (mapping_size.has_overflow())
        return Error::from_errno(EOVERFLOW);

    auto* mapping = TRY(Core::System::mmap(nullptr, mapping_size.value(), protection, flags, fd, page_aligned_offset, 0, path));
    auto* data = reinterpret_cast<u8*>(mapping) + offset_in_mapping;

    return adopt_own(*new MappedFile(mapping, mapping_size.value(), data, size, mode));
}

MappedFile::MappedFile(void* mapping, size_t mapping_size, void* data, size_t size, Mode mode)
    : FixedMemoryStream(Bytes { data, size }, mode)
    , m_mapping(mapping)
    , m_mapping_size(mapping_size)
    , m_data(data)
    , m_size(size)
{
}

MappedFile::~MappedFile()
{
    if (!m_mapping)
        return;

    auto res = Core::System::munmap(m_mapping, m_mapping_size);
    if (res.is_error())
        dbgln("Failed to unmap MappedFile (@ {:p}): {}", m_mapping, res.error());
}

}
