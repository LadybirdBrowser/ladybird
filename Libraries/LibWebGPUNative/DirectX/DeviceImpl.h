/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebGPUNative/Device.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>

using namespace Microsoft::WRL;

namespace WebGPUNative {

struct Device::Impl {
    explicit Impl(Adapter const& gpu_adapter);
    ~Impl() = default;

    ErrorOr<void> initialize();

    ComPtr<ID3D12Device> device() const { return m_device; }
    ComPtr<ID3D12CommandQueue> command_queue() const { return m_command_queue; }

private:
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_command_queue;
};

}
