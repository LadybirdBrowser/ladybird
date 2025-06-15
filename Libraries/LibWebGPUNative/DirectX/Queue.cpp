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

ErrorOr<void> Queue::submit(Vector<NonnullRawPtr<CommandBuffer>> const& gpu_command_buffers)
{
    return m_impl->submit(gpu_command_buffers);
}

void Queue::on_submitted(Function<void()> callback)
{
    m_impl->on_submitted(std::move(callback));
}

}
