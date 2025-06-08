/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebGPUNative/CommandBuffer.h>
#include <vulkan/vulkan_core.h>

namespace WebGPUNative {

struct CommandBuffer::Impl {
    explicit Impl(CommandEncoder const& gpu_command_encoder);

    VkCommandBuffer command_buffer() const { return m_command_buffer; }

private:
    VkCommandBuffer m_command_buffer = { VK_NULL_HANDLE };
};

}
