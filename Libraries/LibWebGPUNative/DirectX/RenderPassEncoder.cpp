/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/DirectX/RenderPassEncoderImpl.h>
#include <LibWebGPUNative/RenderPassEncoder.h>

namespace WebGPUNative {

RenderPassEncoder::RenderPassEncoder(CommandEncoder const& gpu_command_encoder, RenderPassDescriptor const& gpu_render_pass_descriptor)
    : m_impl(make<Impl>(gpu_command_encoder, gpu_render_pass_descriptor))
{
}

RenderPassEncoder::RenderPassEncoder(RenderPassEncoder&&) = default;
RenderPassEncoder& RenderPassEncoder::operator=(RenderPassEncoder&&) = default;
RenderPassEncoder::~RenderPassEncoder() = default;

ErrorOr<void> RenderPassEncoder::initialize()
{
    return m_impl->initialize();
}

RenderPassDescriptor const& RenderPassEncoder::render_pass_descriptor() const
{
    return m_impl->render_pass_descriptor();
}

void RenderPassEncoder::end()
{
    m_impl->end();
}

}
