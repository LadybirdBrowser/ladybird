/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebGPUNative/Adapter.h>
#include <vulkan/vulkan_core.h>

namespace WebGPUNative {

struct Adapter::Impl {
    explicit Impl(Instance const& gpu);

    ~Impl();

    ErrorOr<void> initialize();

    VkPhysicalDevice physical_device() const { return m_physical_device; }

private:
    VkInstance m_instance = { VK_NULL_HANDLE };
    VkPhysicalDevice m_physical_device = { VK_NULL_HANDLE };
};

}
