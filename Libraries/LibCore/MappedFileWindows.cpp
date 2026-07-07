/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, kleines Filmröllchen <filmroellchen@serenityos.org>
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/ScopeGuard.h>
#include <LibCore/File.h>
#include <LibCore/MappedFile.h>
#include <LibCore/System.h>

#include <AK/Windows.h>

namespace Core {

static constexpr size_t to_end_of_file = NumericLimits<size_t>::max();

ErrorOr<NonnullOwnPtr<MappedFile>> MappedFile::map(StringView path, Mode mode)
{
    auto open_mode = mode == Mode::ReadOnly
        ? File::OpenMode::Read
        : File::OpenMode::ReadWrite | File::OpenMode::DontCreate;
    auto file = TRY(File::open(path, open_mode));
    return map_from_fd_and_close(file->leak_fd(), path, mode);
}

ErrorOr<NonnullOwnPtr<MappedFile>> MappedFile::map_from_file(NonnullOwnPtr<Core::File> stream, StringView path)
{
    return map_from_fd_and_close(stream->leak_fd(), path);
}

ErrorOr<NonnullOwnPtr<MappedFile>> MappedFile::map_from_fd_and_close(int fd, StringView path, Mode mode)
{
    return map_from_fd_range_and_close(fd, path, 0, to_end_of_file, mode);
}

ErrorOr<NonnullOwnPtr<MappedFile>> MappedFile::map_from_fd_range_and_close(int fd, StringView, off_t offset, size_t size, Mode mode)
{
    ScopeGuard fd_close_guard = [fd] {
        (void)System::close(fd);
    };

    if (offset < 0 || !AK::is_within_range<size_t>(offset))
        return Error::from_errno(EINVAL);

    LARGE_INTEGER large_file_size = {};
    if (!GetFileSizeEx(to_handle(fd), &large_file_size))
        return Error::from_windows_error();

    auto file_size = static_cast<size_t>(large_file_size.QuadPart);
    auto requested_offset = static_cast<size_t>(offset);
    if (requested_offset > file_size)
        return Error::from_errno(EINVAL);
    if (size == to_end_of_file)
        size = file_size - requested_offset;
    else if (size > file_size - requested_offset)
        return Error::from_errno(EINVAL);

    if (size == 0)
        return adopt_own(*new MappedFile(nullptr, 0, nullptr, 0, mode));

    // MapViewOfFile requires the offset to be aligned to the system allocation granularity.
    auto aligned_offset = align_down_to(requested_offset, system_allocation_granularity());
    auto offset_in_mapping = requested_offset - aligned_offset;

    Checked<size_t> view_size = offset_in_mapping;
    view_size += size;
    if (view_size.has_overflow())
        return Error::from_errno(EOVERFLOW);

    // Like the POSIX backend, read-only mappings are shared and writable mappings are
    // copy-on-write, so writes through a MappedFile never reach the underlying file.
    HANDLE file_mapping = CreateFileMappingW(to_handle(fd), nullptr, mode == Mode::ReadOnly ? PAGE_READONLY : PAGE_WRITECOPY, 0, 0, nullptr);
    if (!file_mapping)
        return Error::from_windows_error();
    ScopeGuard file_mapping_guard = [&] { CloseHandle(file_mapping); };

    void* view = MapViewOfFile(file_mapping, mode == Mode::ReadOnly ? FILE_MAP_READ : FILE_MAP_COPY, static_cast<DWORD>(aligned_offset >> 32), static_cast<DWORD>(aligned_offset & 0xFFFFFFFF), view_size.value());
    if (!view)
        return Error::from_windows_error();

    auto* data = static_cast<u8*>(view) + offset_in_mapping;
    return adopt_own(*new MappedFile(view, view_size.value(), data, size, mode));
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

    if (!UnmapViewOfFile(m_mapping))
        dbgln("Failed to unmap MappedFile (@ {:p}, {} bytes): {}", m_mapping, m_mapping_size, Error::from_windows_error());
}

}
