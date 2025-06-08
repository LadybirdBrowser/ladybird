/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-License: BSD-2-Clause
 */

#include <LibWebGPUNative/Metal/CommandEncoderImpl.h>
#include <LibWebGPUNative/Metal/DeviceImpl.h>
#include <LibWebGPUNative/Metal/Error.h>
#import <Metal/Metal.h>

namespace WebGPUNative {

CommandEncoder::Impl::Impl(Device const& gpu_device)
{
    id command_queue = gpu_device.m_impl->mtl_command_queue();
    if (command_queue) {
        m_command_queue = make<MetalCommandQueueHandle>(command_queue);
    }
}

ErrorOr<void> CommandEncoder::Impl::initialize()
{
    if (!m_command_queue) {
        return make_error("No command queue available");
    }

    id<MTLCommandBuffer> command_buffer = [m_command_queue->get() commandBuffer];
    if (!command_buffer) {
        return make_error("Unable to create command buffer");
    }

    m_command_buffer = make<MetalCommandBufferHandle>(command_buffer);

    return {};
}

}
