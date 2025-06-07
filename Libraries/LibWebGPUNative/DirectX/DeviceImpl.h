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

private:
    ComPtr<IDXGIAdapter4> m_adapter;
    ComPtr<ID3D12Device> m_device;
};

}
