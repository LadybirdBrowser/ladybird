/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/Vulkan/CommandBufferImpl.h>
#include <LibWebGPUNative/Vulkan/CommandEncoderImpl.h>

namespace WebGPUNative {

CommandBuffer::Impl::Impl(CommandEncoder const& gpu_command_encoder)
    : m_command_buffer(gpu_command_encoder.m_impl->command_buffer())
{
}

}
