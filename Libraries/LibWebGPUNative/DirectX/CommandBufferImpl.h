/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebGPUNative/CommandBuffer.h>
#include <d3d12.h>
#include <wrl.h>

using namespace Microsoft::WRL;

namespace WebGPUNative {

struct CommandBuffer::Impl {
    explicit Impl(CommandEncoder const& gpu_command_encoder);

    ComPtr<ID3D12GraphicsCommandList> command_buffer() const { return m_command_list; }

private:
    ComPtr<ID3D12GraphicsCommandList> m_command_list;
};

}
