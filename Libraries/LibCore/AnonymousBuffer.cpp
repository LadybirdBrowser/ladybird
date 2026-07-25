/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Try.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/System.h>
#include <fcntl.h>
#include <sys/mman.h>

namespace Core {

ErrorOr<AnonymousBuffer> AnonymousBuffer::create_with_size(size_t size)
{
    auto fd = TRY(Core::System::anon_create(size, O_CLOEXEC));
    return create_from_anon_fd(fd, size);
}

ErrorOr<NonnullRefPtr<AnonymousBufferImpl>> AnonymousBufferImpl::create(int fd, size_t size)
{
    void* data = nullptr;
    // POSIX mmap rejects a zero length with EINVAL, so leave m_data null for zero-size buffers.
    if (size > 0) {
        data = mmap(nullptr, round_up_to_power_of_two(size, PAGE_SIZE), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) {
            auto error = Error::from_errno(errno);
            close(fd);
            return error;
        }
    }
    auto impl_or_error = AK::adopt_nonnull_ref_or_enomem(new (nothrow) AnonymousBufferImpl(fd, size, data));
    if (impl_or_error.is_error()) {
        close(fd);
        if (data != nullptr)
            munmap(data, round_up_to_power_of_two(size, PAGE_SIZE));
    }
    return impl_or_error;
}

AnonymousBufferImpl::~AnonymousBufferImpl()
{
    if (m_fd != -1) {
        auto rc = close(m_fd);
        VERIFY(rc == 0);
    }
    if (m_data) {
        auto rc = munmap(m_data, round_up_to_power_of_two(m_size, PAGE_SIZE));
        VERIFY(rc == 0);
    }
}

ErrorOr<AnonymousBuffer> AnonymousBuffer::create_from_anon_fd(int fd, size_t size)
{
    auto impl = TRY(AnonymousBufferImpl::create(fd, size));
    return AnonymousBuffer(move(impl));
}

AnonymousBufferImpl::AnonymousBufferImpl(int fd, size_t size, void* data)
    : m_fd(fd)
    , m_size(size)
    , m_data(data)
{
}

}
