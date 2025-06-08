/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebGPUNative/CommandEncoder.h>
#include <d3d12.h>
#include <wrl.h>

using namespace Microsoft::WRL;

namespace WebGPUNative {

struct CommandEncoder::Impl {
    explicit Impl(Device const& gpu_device);

    ErrorOr<void> initialize();

    ComPtr<ID3D12GraphicsCommandList> command_list() const { return m_command_list; }

private:
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandAllocator> m_command_allocator;
    ComPtr<ID3D12GraphicsCommandList> m_command_list;
};

}
