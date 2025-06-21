/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/DirectX/CommandEncoderImpl.h>
#include <LibWebGPUNative/DirectX/Error.h>
#include <LibWebGPUNative/DirectX/RenderPassEncoderImpl.h>

namespace WebGPUNative {

RenderPassEncoder::Impl::Impl(CommandEncoder const& gpu_command_encoder, RenderPassDescriptor const& gpu_render_pass_descriptor)
    : m_command_list(gpu_command_encoder.m_impl->command_list())
    , m_render_pass_descriptor(gpu_render_pass_descriptor)
{
}

// NOTE: There is no explicit render pass object to create for DirectX, everything is through the command list
ErrorOr<void> RenderPassEncoder::Impl::initialize()
{
    return {};
}

void RenderPassEncoder::Impl::end()
{
}

}
