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

}
