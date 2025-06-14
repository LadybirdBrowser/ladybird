/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/Metal/AdapterImpl.h>
#include <LibWebGPUNative/Metal/DeviceImpl.h>
#include <LibWebGPUNative/Metal/Error.h>
#include <LibWebGPUNative/include/LibWebGPUNative-Swift.h>
#include <Metal/Metal.h>

namespace WebGPUNative {

struct Device::Impl::SwiftImpl {
    explicit SwiftImpl(id mtl_device)
        : m_mtl_device(mtl_device)
        , m_bridge(make<WebGPUNative::DeviceImpl>(WebGPUNative::DeviceImpl::init((__bridge void*)m_mtl_device)))
    {
    }
    id m_mtl_device;
    NonnullOwnPtr<WebGPUNative::DeviceImpl> m_bridge;
};

Device::Impl::Impl(Adapter const& gpu_adapter)
    : m_swift_impl(make<SwiftImpl>(gpu_adapter.m_impl->mtl_device()))
{
}

Device::Impl::~Impl() = default;

id Device::Impl::mtl_device() const
{
    return m_swift_impl->m_mtl_device;
}

id Device::Impl::mtl_command_queue() const
{
    return (__bridge id<MTLCommandQueue>)m_swift_impl->m_bridge->getCommandQueue();
}

ErrorOr<void> Device::Impl::initialize()
{
    auto const maybe_error = m_swift_impl->m_bridge->initialize();
    if (maybe_error) {
        auto device_error = maybe_error.getSome();
        auto error_message = device_error.getMessage();
        NSString const* ns_string = error_message;
        char const* c_string = [ns_string UTF8String];
        return Error::from_string_view(StringView { c_string, strlen(c_string) });
    }
    return {};
}

}
