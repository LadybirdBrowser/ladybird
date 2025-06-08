/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/CommandBuffer.h>
#include <LibWebGPUNative/CommandEncoder.h>
#include <LibWebGPUNative/RenderPassEncoder.h>
#include <LibWebGPUNative/Vulkan/CommandEncoderImpl.h>

namespace WebGPUNative {

CommandEncoder::CommandEncoder(Device const& gpu_device)
    : m_impl(make<Impl>(gpu_device))
{
}

CommandEncoder::CommandEncoder(CommandEncoder&&) noexcept = default;
CommandEncoder& CommandEncoder::operator=(CommandEncoder&&) noexcept = default;
CommandEncoder::~CommandEncoder() = default;

ErrorOr<void> CommandEncoder::initialize()
{
    return m_impl->initialize();
}

ErrorOr<RenderPassEncoder> CommandEncoder::begin_render_pass(RenderPassDescriptor const& render_pass_descriptor) const
{
    auto render_pass_encoder = RenderPassEncoder(*this, render_pass_descriptor);
    TRY(render_pass_encoder.initialize());
    TRY(m_impl->begin_render_pass(render_pass_encoder));
    return render_pass_encoder;
}

ErrorOr<CommandBuffer> CommandEncoder::finish()
{
    TRY(m_impl->finish());
    return CommandBuffer(*this);
}

}
