/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/Queue.h>
#include <LibWebGPUNative/Vulkan/QueueImpl.h>

namespace WebGPUNative {

Queue::Queue(Device const& gpu_device)
    : m_impl(make<Impl>(gpu_device))
{
}

Queue::Queue(Queue&&) noexcept = default;
Queue& Queue::operator=(Queue&&) noexcept = default;
Queue::~Queue() = default;

}
