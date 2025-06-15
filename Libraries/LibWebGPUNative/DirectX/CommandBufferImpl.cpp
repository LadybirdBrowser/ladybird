/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/DirectX/CommandBufferImpl.h>
#include <LibWebGPUNative/DirectX/CommandEncoderImpl.h>

namespace WebGPUNative {

CommandBuffer::Impl::Impl(CommandEncoder const& gpu_command_encoder)
    : m_command_list(gpu_command_encoder.m_impl->command_list())
{
}

}
