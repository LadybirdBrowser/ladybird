/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebGPUNative/CommandEncoder.h>
#include <vulkan/vulkan_core.h>

namespace WebGPUNative {

struct CommandEncoder::Impl {
    explicit Impl(Device const& gpu_device);

    ~Impl();

    ErrorOr<void> initialize();

    VkDevice logical_device() const { return m_logical_device; }

    VkCommandBuffer command_buffer() const { return m_command_buffer; }

private:
    VkDevice m_logical_device = { VK_NULL_HANDLE };
    VkCommandPool m_command_pool = { VK_NULL_HANDLE };
    VkCommandBuffer m_command_buffer = { VK_NULL_HANDLE };
};

}
