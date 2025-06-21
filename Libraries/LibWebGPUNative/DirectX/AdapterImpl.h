/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebGPUNative/Adapter.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>

using namespace Microsoft::WRL;

namespace WebGPUNative {

struct Adapter::Impl {
    explicit Impl() = default;
    ~Impl();

    ErrorOr<void> initialize();

    ComPtr<ID3D12Device> device() const { return m_device; }

private:
    ComPtr<IDXGIFactory6> m_factory;
    ComPtr<IDXGIAdapter4> m_adapter;
    ComPtr<ID3D12Device> m_device;
};

}
