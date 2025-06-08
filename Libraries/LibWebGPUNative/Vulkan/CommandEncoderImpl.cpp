/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/Vulkan/CommandEncoderImpl.h>
#include <LibWebGPUNative/Vulkan/DeviceImpl.h>
#include <LibWebGPUNative/Vulkan/Error.h>

namespace WebGPUNative {

CommandEncoder::Impl::Impl(Device const& gpu_device)
    : m_logical_device(gpu_device.m_impl->logical_device())
    , m_command_pool(gpu_device.m_impl->command_pool())
{
}

CommandEncoder::Impl::~Impl()
{
    vkFreeCommandBuffers(m_logical_device, m_command_pool, 1, &m_command_buffer);
}

ErrorOr<void> CommandEncoder::Impl::initialize()
{
    VkCommandBufferAllocateInfo command_buffer_allocate_info = {};
    command_buffer_allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_buffer_allocate_info.commandPool = m_command_pool;
    command_buffer_allocate_info.commandBufferCount = 1;

    if (VkResult const allocate_command_buffers_result = vkAllocateCommandBuffers(m_logical_device, &command_buffer_allocate_info, &m_command_buffer); allocate_command_buffers_result != VK_SUCCESS)
        return make_error(allocate_command_buffers_result, "Unable to allocate command buffers");

    VkCommandBufferBeginInfo command_buffer_begin_info = {};
    command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (VkResult const begin_command_buffer_result = vkBeginCommandBuffer(m_command_buffer, &command_buffer_begin_info); begin_command_buffer_result != VK_SUCCESS)
        return make_error(begin_command_buffer_result, "Unable to begin command buffer");
    return {};
}

}
