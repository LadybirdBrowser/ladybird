/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/Metal/CommandBufferImpl.h>
#import <Metal/Metal.h>

namespace WebGPUNative {

CommandBuffer::Impl::Impl(CommandEncoder const& gpu_command_encoder)
{
    id command_buffer = gpu_command_encoder.m_impl->command_buffer();
    if (command_buffer) {
        m_command_buffer = make<MetalCommandBufferHandle>(command_buffer);
    }
}

}
