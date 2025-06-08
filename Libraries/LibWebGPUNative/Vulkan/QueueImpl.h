/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebGPUNative/Queue.h>
#include <vulkan/vulkan_core.h>

namespace WebGPUNative {

struct Queue::Impl {
    explicit Impl(Device const& gpu_device);

private:
    VkQueue m_queue = { VK_NULL_HANDLE };
};

}
