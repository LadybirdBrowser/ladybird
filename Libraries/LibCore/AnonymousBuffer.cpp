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
#include <unistd.h>

#if defined(AK_OS_MACOS)
#    include <mach/mach.h>
#    include <mach/mach_vm.h>
#endif

namespace Core {

ErrorOr<AnonymousBuffer> AnonymousBuffer::create_with_size(size_t size, Sealability sealability)
{
    auto allow_sealing = sealability == Sealability::Sealable
        ? Core::System::AllowSealing::Yes
        : Core::System::AllowSealing::No;
    auto fd = TRY(Core::System::anon_create(size, O_CLOEXEC, allow_sealing));
    return create_from_anon_fd(fd, size);
}

ErrorOr<AnonymousBuffer> AnonymousBuffer::create_from_anon_fd(int fd, size_t size)
{
    auto impl = TRY(AnonymousBufferImpl::create(fd, size));
    return AnonymousBuffer(move(impl));
}

ErrorOr<AnonymousBuffer> AnonymousBuffer::snapshot(Sealability sealability) const
{
    if (!is_valid())
        return Error::from_string_literal("Cannot snapshot an invalid anonymous buffer");

    bool is_size_sealed = false;

#if defined(F_ADD_SEALS) && defined(F_GET_SEALS) && defined(F_SEAL_GROW) && defined(F_SEAL_SHRINK) && defined(F_SEAL_SEAL)
    static constexpr auto REQUIRED_SEALS = F_SEAL_GROW | F_SEAL_SHRINK;

    auto seals = TRY(Core::System::fcntl(fd(), F_GET_SEALS, static_cast<uintptr_t>(0)));

    if ((seals & REQUIRED_SEALS) != REQUIRED_SEALS && !(seals & F_SEAL_SEAL)) {
        TRY(Core::System::fcntl(fd(), F_ADD_SEALS, REQUIRED_SEALS));
        seals |= REQUIRED_SEALS;
    }

    is_size_sealed = (seals & REQUIRED_SEALS) == REQUIRED_SEALS;
#endif

    auto file_status = TRY(Core::System::fstat(fd()));
    if (file_status.st_size < 0 || static_cast<u64>(file_status.st_size) < size())
        return Error::from_string_literal("Anonymous buffer is smaller than its claimed size");

    auto copy = TRY(create_with_size(size(), sealability));
    if (size() == 0)
        return copy;

    if (is_size_sealed) {
        bytes().copy_to({ copy.data<u8>(), copy.size() });
        return copy;
    }

#if defined(AK_OS_MACOS)
    // POSIX shared memory descriptors reject pread() on macOS. Copy through Mach so a concurrent truncation is reported
    // as an error instead of raising SIGBUS while reading the mapping directly.
    mach_vm_size_t copied_size = 0;

    auto result = mach_vm_read_overwrite(mach_task_self(),
        reinterpret_cast<mach_vm_address_t>(data<void>()), size(),
        reinterpret_cast<mach_vm_address_t>(copy.data<void>()), &copied_size);

    if (result != KERN_SUCCESS || copied_size != size())
        return Error::from_string_literal("Failed to snapshot anonymous buffer");
#else
    // Unlike bytes().copy_to(), pread() reports a concurrent truncation as a short read instead of raising SIGBUS
    // while reading the unsealed mapping.
    size_t copied_size = 0;

    while (copied_size < size()) {
        auto bytes_read = ::pread(fd(), copy.data<u8>() + copied_size, size() - copied_size, static_cast<off_t>(copied_size));
        if (bytes_read < 0) {
            if (errno == EINTR)
                continue;
            return Error::from_errno(errno);
        }

        if (bytes_read == 0)
            return Error::from_string_literal("Anonymous buffer was truncated while being snapshotted");
        copied_size += static_cast<size_t>(bytes_read);
    }
#endif

    return copy;
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

AnonymousBufferImpl::AnonymousBufferImpl(int fd, size_t size, void* data)
    : m_fd(fd)
    , m_size(size)
    , m_data(data)
{
}

}
