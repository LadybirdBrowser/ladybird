/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/Metal/AdapterImpl.h>
#include <LibWebGPUNative/Metal/Error.h>
#include <LibWebGPUNative/include/LibWebGPUNative-Swift.h>
#include <Metal/Metal.h>

namespace WebGPUNative {

struct Adapter::Impl::SwiftImpl {
    explicit SwiftImpl()
        : m_bridge(make<AdapterImpl>(AdapterImpl::init()))
    {
    }

    NonnullOwnPtr<AdapterImpl> m_bridge;
};

Adapter::Impl::Impl(Instance const&)
    : m_swift_impl(make<SwiftImpl>())
{
}

Adapter::Impl::~Impl() = default;

id Adapter::Impl::mtl_device() const
{
    return (__bridge id<MTLDevice>)m_swift_impl->m_bridge->getMetalDevice();
}

ErrorOr<void> Adapter::Impl::initialize()
{
    auto const maybe_error = m_swift_impl->m_bridge->initialize();
    if (maybe_error) {
        auto adapter_error = maybe_error.getSome();
        auto error_message = adapter_error.getMessage();
        NSString const* ns_string = error_message;
        char const* c_string = [ns_string UTF8String];
        return Error::from_string_view(StringView { c_string, strlen(c_string) });
    }
    return {};
}

}
