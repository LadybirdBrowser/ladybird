/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/DirectX/QueueImpl.h>
#include <LibWebGPUNative/Queue.h>

namespace WebGPUNative {

Queue::Queue(Device const& gpu_device)
    : m_impl(make<Impl>(gpu_device))
{
}

Queue::Queue(Queue&&) noexcept = default;
Queue& Queue::operator=(Queue&&) noexcept = default;
Queue::~Queue() = default;

}
