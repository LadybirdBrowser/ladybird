/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/DirectX/AdapterImpl.h>
#include <LibWebGPUNative/DirectX/Error.h>
#include <d3d12.h>

namespace WebGPUNative {

Adapter::Impl::~Impl() = default;

ErrorOr<void> Adapter::Impl::initialize()
{
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory)))) {
        return make_error("Unable to create factory");
    }

    for (UINT i = 0; m_factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&m_adapter)) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC3 desc;
        m_adapter->GetDesc3(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE)
            continue;
        if (SUCCEEDED(D3D12CreateDevice(m_adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr))) {
            break;
        }
        m_adapter.Reset();
    }

    return {};
}

}
