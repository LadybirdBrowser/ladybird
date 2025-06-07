/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/Metal/AdapterImpl.h>
#include <LibWebGPUNative/include/LibWebGPUNative-Swift.h>

#include <LibWebGPUNative/Metal/Error.h>

namespace WebGPUNative {

AdapterImplBridge::AdapterImplBridge()
{
    auto swift_adapter = WebGPUNative::AdapterImpl::init();
    m_metal_device = new WebGPUNative::AdapterImpl(std::move(swift_adapter));
}

AdapterImplBridge::~AdapterImplBridge()
{
    if (m_metal_device) {
        delete static_cast<WebGPUNative::AdapterImpl*>(m_metal_device);
    }
}

bool AdapterImplBridge::initialize()
{
    auto* swift_adapter = static_cast<WebGPUNative::AdapterImpl*>(m_metal_device);
    return swift_adapter->initialize();
}

id AdapterImplBridge::metal_device()
{
    auto* swift_adapter = static_cast<WebGPUNative::AdapterImpl*>(m_metal_device);
    return static_cast<id>(swift_adapter->getMetalDevice());
}

ErrorOr<void> Adapter::Impl::initialize()
{
    bool const success = adapter_bridge.initialize();
    if (!success) {
        return make_error("Failed to initialize Metal adapter");
    }

    m_metal_device = adapter_bridge.metal_device();
    return {};
}

}
