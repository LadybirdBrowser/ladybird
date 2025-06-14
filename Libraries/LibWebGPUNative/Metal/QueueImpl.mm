/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-License: BSD-2-Clause
 */

#include <LibWebGPUNative/Metal/CommandBufferImpl.h>
#include <LibWebGPUNative/Metal/DeviceImpl.h>
#include <LibWebGPUNative/Metal/Error.h>
#include <LibWebGPUNative/Metal/QueueImpl.h>
#import <Metal/Metal.h>

namespace WebGPUNative {

Queue::Impl::Impl(Device const& gpu_device)
    : m_command_queue(gpu_device.m_impl->mtl_command_queue())
{
}

// FIXME: Add spec comments
//  https://www.w3.org/TR/webgpu/#dom-gpuqueue-submit
ErrorOr<void> Queue::Impl::submit(Vector<NonnullRawPtr<CommandBuffer>> const& gpu_command_buffers)
{
    if (!m_command_queue) {
        return make_error("No Metal command queue available");
    }

    for (auto const& command_buffer : gpu_command_buffers) {
        id<MTLCommandBuffer> metal_command_buffer = static_cast<id<MTLCommandBuffer>>(command_buffer->m_impl->command_buffer());
        if (!metal_command_buffer) {
            return make_error("Invalid Metal command buffer");
        }

        if (m_submitted_callback) {
            // TODO: Consult spec for the recommended method of notifying the GPUCanvasContext that it needs to update its HTMLCanvasElement surface
            [metal_command_buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
                m_submitted_callback();
            }];
        }

        [metal_command_buffer commit];

        // TODO: Make queue submission asynchronous
        // https://www.w3.org/TR/webgpu/#dom-gpuqueue-onsubmittedworkdone
        [metal_command_buffer waitUntilCompleted];
    }

    return {};
}

void Queue::Impl::on_submitted(Function<void()> callback)
{
    m_submitted_callback = std::move(callback);
}

}
