/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Vector.h>
#include <LibWebGPUNative/DirectX/DeviceImpl.h>
#include <LibWebGPUNative/Queue.h>
#include <d3d12.h>
#include <wrl.h>

using namespace Microsoft::WRL;

namespace WebGPUNative {

struct Queue::Impl {
    explicit Impl(Device const& gpu_device);

    ErrorOr<void> submit(Vector<NonnullRawPtr<CommandBuffer>> const&);
    void on_submitted(Function<void()> callback);

private:
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_command_queue;
    Function<void()> m_submitted_callback = { nullptr };
};

}
