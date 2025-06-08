/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebGPUNative/RenderPassEncoder.h>
#include <d3d12.h>
#include <wrl.h>

using namespace Microsoft::WRL;

namespace WebGPUNative {

struct RenderPassEncoder::Impl {
    explicit Impl(CommandEncoder const& gpu_command_encoder, RenderPassDescriptor const&);

    ErrorOr<void> initialize();

    ComPtr<ID3D12GraphicsCommandList> command_list() const { return m_command_list; }

    RenderPassDescriptor const& render_pass_descriptor() const { return m_render_pass_descriptor; }

    void end();

private:
    ComPtr<ID3D12GraphicsCommandList> m_command_list;
    RenderPassDescriptor m_render_pass_descriptor;
};

}
