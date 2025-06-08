/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/DirectX/CommandEncoderImpl.h>
#include <LibWebGPUNative/DirectX/DeviceImpl.h>
#include <LibWebGPUNative/DirectX/Error.h>

namespace WebGPUNative {

CommandEncoder::Impl::Impl(Device const& gpu_device)
    : m_device(gpu_device.m_impl->device())
    , m_command_allocator(gpu_device.m_impl->command_allocator())
{
}

ErrorOr<void> CommandEncoder::Impl::initialize()
{
    if (HRESULT const result = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_command_allocator.Get(), nullptr, IID_PPV_ARGS(&m_command_list)); FAILED(result))
        return make_error(result, "Unable to create command list");
    return {};
}

}
