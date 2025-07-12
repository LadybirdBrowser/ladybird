/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/CommandBuffer.h>
#include <LibWebGPUNative/Vulkan/CommandBufferImpl.h>

namespace WebGPUNative {

CommandBuffer::CommandBuffer(CommandEncoder const& gpu_command_encoder)
    : m_impl(make<Impl>(gpu_command_encoder))
{
}

CommandBuffer::CommandBuffer(CommandBuffer&&) noexcept = default;
CommandBuffer& CommandBuffer::operator=(CommandBuffer&&) noexcept = default;
CommandBuffer::~CommandBuffer() = default;

}
