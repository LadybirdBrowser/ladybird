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
    if (HRESULT const result = CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory)); FAILED(result))
        return make_error(result, "Unable to create factory");

    // FIXME: Expose and acknowledge options for guiding adapter selection
    //  https://www.w3.org/TR/webgpu/#adapter-selection
    for (UINT i = 0; m_factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&m_adapter)) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC3 desc;
        if (HRESULT const result = m_adapter->GetDesc3(&desc); FAILED(result))
            return make_error(result, "Unable to get adapter description");
        if (desc.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE)
            continue;
        if (HRESULT const result = D3D12CreateDevice(m_adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device)); SUCCEEDED(result)) {
            break;
        } else {
            m_adapter.Reset();
            return make_error(result, "Unable to create D3D12 device");
        }
    }

    return {};
}

}
