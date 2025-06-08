/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibWebGPUNative/Metal/Handle.h>
#include <LibWebGPUNative/RenderPassEncoder.h>

namespace WebGPUNative {

struct RenderPassEncoder::Impl {
    explicit Impl(CommandEncoder const& gpu_command_encoder, RenderPassDescriptor const& gpu_render_pass_descriptor);
    ~Impl() = default;

    RenderPassDescriptor const& render_pass_descriptor() const { return m_render_pass_descriptor; }
    id command_buffer() const { return m_command_buffer->get(); }

    void end();

private:
    OwnPtr<MetalCommandBufferHandle> m_command_buffer;
    RenderPassDescriptor m_render_pass_descriptor;
};

} // namespace WebGPUNative
