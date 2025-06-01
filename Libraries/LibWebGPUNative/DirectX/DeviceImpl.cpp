/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-License: BSD-2-Clause
 */

#include <LibWebGPUNative/DirectX/AdapterImpl.h>
#include <LibWebGPUNative/DirectX/DeviceImpl.h>
#include <LibWebGPUNative/DirectX/Error.h>

namespace WebGPUNative {

Device::Impl::Impl(Adapter const& gpu_adapter)
    : m_adapter(gpu_adapter.m_impl->adapter())
{
}

ErrorOr<void> Device::Impl::initialize()
{
    if (FAILED(D3D12CreateDevice(m_adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device))))
        return make_error("Unable to create adapter");

    return {};
}

}
