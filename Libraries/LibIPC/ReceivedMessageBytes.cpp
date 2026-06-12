/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/RefCounted.h>
#include <LibIPC/ReceivedMessageBytes.h>

#if defined(AK_OS_MACOS)
#    include <mach/mach.h>
#endif

namespace IPC {

class ReceivedMessageBytes::Impl final : public RefCounted<ReceivedMessageBytes::Impl> {
public:
    explicit Impl(Vector<u8> bytes)
        : m_storage_type(StorageType::Vector)
        , m_vector(move(bytes))
    {
    }

#if defined(AK_OS_MACOS)
    Impl(void* vm_region_address, size_t vm_region_size)
        : m_storage_type(StorageType::VMRegion)
        , m_vm_region_address(vm_region_address)
        , m_vm_region_size(vm_region_size)
    {
    }
#endif

    ~Impl()
    {
#if defined(AK_OS_MACOS)
        if (m_storage_type == StorageType::VMRegion && m_vm_region_size > 0)
            vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(m_vm_region_address), m_vm_region_size);
#endif
    }

    ReadonlyBytes bytes() const
    {
        switch (m_storage_type) {
        case StorageType::Vector:
            return m_vector;
        case StorageType::VMRegion:
            return { static_cast<u8 const*>(m_vm_region_address), m_vm_region_size };
        }
        VERIFY_NOT_REACHED();
    }

private:
    enum class StorageType {
        Vector,
        VMRegion,
    };

    StorageType m_storage_type { StorageType::Vector };
    Vector<u8> m_vector;
    void* m_vm_region_address { nullptr };
    size_t m_vm_region_size { 0 };
};

ReceivedMessageBytes::ReceivedMessageBytes() = default;
ReceivedMessageBytes::ReceivedMessageBytes(ReceivedMessageBytes const&) = default;
ReceivedMessageBytes::ReceivedMessageBytes(ReceivedMessageBytes&&) = default;
ReceivedMessageBytes::~ReceivedMessageBytes() = default;

ReceivedMessageBytes& ReceivedMessageBytes::operator=(ReceivedMessageBytes const&) = default;
ReceivedMessageBytes& ReceivedMessageBytes::operator=(ReceivedMessageBytes&&) = default;

ReceivedMessageBytes::ReceivedMessageBytes(NonnullRefPtr<Impl> impl)
    : m_impl(move(impl))
{
}

ReceivedMessageBytes ReceivedMessageBytes::from_vector(Vector<u8> bytes)
{
    if (bytes.is_empty())
        return {};
    return ReceivedMessageBytes { adopt_ref(*new Impl(move(bytes))) };
}

#if defined(AK_OS_MACOS)
ReceivedMessageBytes ReceivedMessageBytes::adopt_vm_region(void* address, size_t size)
{
    if (size == 0)
        return {};
    return ReceivedMessageBytes { adopt_ref(*new Impl(address, size)) };
}
#endif

ReadonlyBytes ReceivedMessageBytes::bytes() const
{
    if (!m_impl)
        return {};
    return m_impl->bytes();
}

}
