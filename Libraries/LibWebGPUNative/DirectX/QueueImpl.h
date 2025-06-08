/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebGPUNative/DirectX/DeviceImpl.h>

#include <LibWebGPUNative/Queue.h>
#include <d3d12.h>
#include <wrl.h>

using namespace Microsoft::WRL;

namespace WebGPUNative {

struct Queue::Impl {
    explicit Impl(Device const& gpu_device);

private:
    ComPtr<ID3D12CommandQueue> m_command_queue;
};

}
