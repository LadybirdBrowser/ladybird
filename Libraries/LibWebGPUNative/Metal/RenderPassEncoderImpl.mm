/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/Metal/CommandEncoderImpl.h>
#include <LibWebGPUNative/Metal/RenderPassEncoderImpl.h>
#import <Metal/Metal.h>

namespace WebGPUNative {

RenderPassEncoder::Impl::Impl(CommandEncoder const& gpu_command_encoder, RenderPassDescriptor const& gpu_render_pass_descriptor)
    : m_command_buffer(make<MetalCommandBufferHandle>(gpu_command_encoder.m_impl->command_buffer()))
    , m_render_pass_descriptor(gpu_render_pass_descriptor)
{
}

void RenderPassEncoder::Impl::end()
{
    // Encoding is ended by CommandEncoderImpl::finish
}

}
