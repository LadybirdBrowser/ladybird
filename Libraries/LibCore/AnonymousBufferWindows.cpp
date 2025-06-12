/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2025, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/AnonymousBuffer.h>
#include <LibCore/System.h>

#include <AK/Windows.h>

namespace Core {

AnonymousBufferImpl::AnonymousBufferImpl(int fd, size_t size, void* data)
    : m_fd(fd)
    , m_size(size)
    , m_data(data)
{
}

AnonymousBufferImpl::~AnonymousBufferImpl()
{
    if (m_data)
        VERIFY(UnmapViewOfFile(m_data));

    if (m_fd != -1)
        MUST(System::close(m_fd));
}

ErrorOr<NonnullRefPtr<AnonymousBufferImpl>> AnonymousBufferImpl::create(size_t size)
{
    HANDLE map_handle = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, size >> 32, size & 0xFFFFFFFF, NULL);
    if (!map_handle)
        return Error::from_windows_error();

    return create(to_fd(map_handle), size);
}

ErrorOr<NonnullRefPtr<AnonymousBufferImpl>> AnonymousBufferImpl::create(int fd, size_t size)
{
    void* ptr = MapViewOfFile(to_handle(fd), FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!ptr)
        return Error::from_windows_error();

    return adopt_ref(*new AnonymousBufferImpl(fd, size, ptr));
}

ErrorOr<AnonymousBuffer> AnonymousBuffer::create_with_size(size_t size)
{
    auto impl = TRY(AnonymousBufferImpl::create(size));
    return AnonymousBuffer(move(impl));
}

ErrorOr<AnonymousBuffer> AnonymousBuffer::create_from_anon_fd(int fd, size_t size)
{
    auto impl = TRY(AnonymousBufferImpl::create(fd, size));
    return AnonymousBuffer(move(impl));
}

}
