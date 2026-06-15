/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/RefPtr.h>
#include <LibGfx/Direct3DContext.h>

#ifdef USE_DIRECTX

#    include <AK/Windows.h>
#    include <d3d12.h>
#    include <dxgi1_4.h>

// NOTE: Not using the newer winrt that supersedes wrl as that uses exceptions for error handling.
#    include <wrl/client.h>

namespace Gfx {

using namespace Microsoft::WRL;

struct Direct3DContext::Impl {
    Impl(ComPtr<IDXGIAdapter1> adapter, ComPtr<ID3D12Device> device, ComPtr<ID3D12CommandQueue> queue)
        : adapter(move(adapter))
        , device(move(device))
        , queue(move(queue))
    {
    }

    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
};

Direct3DContext::Direct3DContext(NonnullOwnPtr<Impl> impl)
    : m_impl(move(impl))
{
}

Direct3DContext::~Direct3DContext() = default;

IDXGIAdapter1* Direct3DContext::adapter() const
{
    return m_impl->adapter.Get();
}

ID3D12Device* Direct3DContext::device() const
{
    return m_impl->device.Get();
}

ID3D12CommandQueue* Direct3DContext::queue() const
{
    return m_impl->queue.Get();
}

static ErrorOr<ComPtr<IDXGIAdapter1>> get_hardware_adapter(IDXGIFactory4& factory)
{
    for (UINT adapter_index = 0;; ++adapter_index) {
        ComPtr<IDXGIAdapter1> adapter;
        auto hr = factory.EnumAdapters1(adapter_index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(hr))
            return Error::from_windows_error(hr);

        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
            return move(adapter);
    }

    return Error::from_string_literal("No Direct3D 12 adapter found");
}

ErrorOr<NonnullRefPtr<Direct3DContext>> create_direct3d_context()
{
    ComPtr<IDXGIFactory4> factory;
    auto hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr))
        return Error::from_windows_error(hr);

    auto adapter = TRY(get_hardware_adapter(*factory.Get()));

    ComPtr<ID3D12Device> device;
    hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr))
        return Error::from_windows_error(hr);

    D3D12_COMMAND_QUEUE_DESC queue_description {};
    queue_description.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    ComPtr<ID3D12CommandQueue> queue;
    hr = device->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&queue));
    if (FAILED(hr))
        return Error::from_windows_error(hr);

    return adopt_ref(*new Direct3DContext(make<Direct3DContext::Impl>(move(adapter), move(device), move(queue))));
}

}

#endif
