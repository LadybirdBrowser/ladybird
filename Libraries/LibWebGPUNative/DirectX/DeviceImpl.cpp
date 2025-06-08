/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/DirectX/AdapterImpl.h>
#include <LibWebGPUNative/DirectX/DeviceImpl.h>
#include <LibWebGPUNative/DirectX/Error.h>

namespace WebGPUNative {

Device::Impl::Impl(Adapter const& gpu_adapter)
    : m_device(gpu_adapter.m_impl->device())
{
}

ErrorOr<void> Device::Impl::initialize()
{
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (HRESULT const result = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_command_queue)); FAILED(result))
        return make_error(result, "Unable to create command queue");

    if (HRESULT const result = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_command_allocator)); FAILED(result))
        return make_error(result, "Unable to create command allocator");
    return {};
}

}
