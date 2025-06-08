/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/DirectX/CommandBufferImpl.h>
#include <LibWebGPUNative/DirectX/DeviceImpl.h>
#include <LibWebGPUNative/DirectX/Error.h>
#include <LibWebGPUNative/DirectX/QueueImpl.h>

namespace WebGPUNative {

Queue::Impl::Impl(Device const& gpu_device)
    : m_device(gpu_device.m_impl->device())
    , m_command_queue(gpu_device.m_impl->command_queue())

{
}

ErrorOr<void> Queue::Impl::submit(Vector<NonnullRawPtr<CommandBuffer>> const& gpu_command_buffers)
{
    Vector<ID3D12CommandList*> command_lists;
    for (auto const& command_buffer : gpu_command_buffers) {
        command_lists.append(command_buffer->m_impl->command_buffer().Get());
    }
    m_command_queue->ExecuteCommandLists(command_lists.size(), command_lists.data());

    ComPtr<ID3D12Fence> fence;
    if (HRESULT const result = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)); FAILED(result))
        return make_error(result, "Unable to create fence for command buffer submission");

    // FIXME: Queue submission should be asynchronous
    //  https://www.w3.org/TR/webgpu/#dom-gpuqueue-onsubmittedworkdone
    HANDLE fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (HRESULT const result = m_command_queue->Signal(fence.Get(), 1); FAILED(result))
        return make_error(result, "Unable to signal fence");

    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, fence_event);
        WaitForSingleObject(fence_event, INFINITE);
    }

    // FIXME: Consult spec for the recommended method of notifying the GPUCanvasContext that is needs to update it's HTMLCanvasElement surface
    if (m_submitted_callback)
        m_submitted_callback();

    return {};
}

void Queue::Impl::on_submitted(Function<void()> callback)
{
    m_submitted_callback = std::move(callback);
}

}
