/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebGPUNative/Device.h>
#include <vulkan/vulkan_core.h>

namespace WebGPUNative {

struct Device::Impl {
    explicit Impl(Adapter const& gpu_adapter);

    ~Impl();

    ErrorOr<void> initialize();

    VkPhysicalDevice physical_device() const { return m_physical_device; }

    VkDevice logical_device() const { return m_logical_device; }

    VkQueue queue() const { return m_queue; }

    VkCommandPool command_pool() const { return m_command_pool; }

private:
    VkPhysicalDevice m_physical_device = { VK_NULL_HANDLE };
    VkDevice m_logical_device = { VK_NULL_HANDLE };
    VkQueue m_queue = { VK_NULL_HANDLE };
    VkCommandPool m_command_pool = { VK_NULL_HANDLE };
};

}
