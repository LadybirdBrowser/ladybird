/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-License: BSD-2-Clause
 */

#include <LibWebGPUNative/Metal/AdapterImpl.h>
#include <LibWebGPUNative/Metal/DeviceImpl.h>
#include <LibWebGPUNative/Metal/Error.h>
#import <Metal/Metal.h>

namespace WebGPUNative {

Device::Impl::Impl(Adapter const& gpu_adapter) {
    id device = gpu_adapter.m_impl->metal_device();
    if (device) {
        m_metal_device = make<MetalDeviceHandle>(device);
    }
}

ErrorOr<void> Device::Impl::initialize() {
    if (!m_metal_device) {
        return make_error("No device available");
    }

    id<MTLCommandQueue> command_queue = [m_metal_device->get() newCommandQueue];
    if (!command_queue) {
        return make_error("Unable to create command queue");
    }

    m_command_queue = make<MetalCommandQueueHandle>(command_queue);

    return {};
}

}
