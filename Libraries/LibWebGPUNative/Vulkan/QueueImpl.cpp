/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/Vulkan/DeviceImpl.h>
#include <LibWebGPUNative/Vulkan/QueueImpl.h>

namespace WebGPUNative {

Queue::Impl::Impl(Device const& gpu_device)
    : m_queue(gpu_device.m_impl->queue())
{
}

// FIXME: Add spec comments
//  https://www.w3.org/TR/webgpu/#dom-gpuqueue-submit
ErrorOr<void> Queue::Impl::submit(Vector<NonnullRawPtr<CommandBuffer>> const& gpu_command_buffers)
{
    Vector<VkCommandBuffer> command_buffers;
    for (auto const& command_buffer : gpu_command_buffers) {
        command_buffers.append(command_buffer->m_impl->command_buffer());
    }

    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = static_cast<uint32_t>(command_buffers.size());
    submit_info.pCommandBuffers = command_buffers.data();
    if (VkResult const queue_submit_result = vkQueueSubmit(m_queue, 1, &submit_info, VK_NULL_HANDLE); queue_submit_result != VK_SUCCESS)
        return make_error(queue_submit_result, "Unable to submit queue");

    // FIXME: Queue submission should be asynchronous
    //  https://www.w3.org/TR/webgpu/#dom-gpuqueue-onsubmittedworkdone
    if (VkResult const queue_wait_idle_result = vkQueueWaitIdle(m_queue); queue_wait_idle_result != VK_SUCCESS)
        return make_error(queue_wait_idle_result, "Unable to wait for queue to be idle");

    // FIXME: Consult spec for the recommended method of notifying the GPUCanvasContext that is needs to update it's HTMLCanvasElement surface
    if (m_submitted_callback)
        m_submitted_callback();

    return {};
}

void Queue::Impl::on_submitted(Function<void()> callback)
{
    m_submitted_callback = std::move(callback);
}

}
