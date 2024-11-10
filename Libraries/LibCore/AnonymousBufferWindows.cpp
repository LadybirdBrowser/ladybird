/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/AnonymousBuffer.h>
#include <windows.h>

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
        VERIFY(CloseHandle((HANDLE)(intptr_t)m_fd));
}

ErrorOr<NonnullRefPtr<AnonymousBufferImpl>> AnonymousBufferImpl::create(size_t size)
{
    HANDLE map_handle = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, size >> 31 >> 1, size & 0xFFFFFFFF, NULL);
    if (!map_handle)
        return Error::from_windows_error(GetLastError());

    return create((int)(intptr_t)map_handle, size);
}

ErrorOr<NonnullRefPtr<AnonymousBufferImpl>> AnonymousBufferImpl::create(int fd, size_t size)
{
    void* ptr = MapViewOfFile((HANDLE)(intptr_t)fd, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!ptr)
        return Error::from_windows_error(GetLastError());

    return adopt_nonnull_ref_or_enomem(new (nothrow) AnonymousBufferImpl(fd, size, ptr));
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
