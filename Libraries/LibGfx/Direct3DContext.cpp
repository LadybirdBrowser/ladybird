/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/NonnullRefPtr.h>
#include <LibGfx/Direct3DContext.h>

#include <windows.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <winrt/base.h>

namespace Gfx {

struct Direct3DContext::Impl {
    winrt::com_ptr<IDXGIFactory1> factory;
    winrt::com_ptr<IDXGIAdapter1> adapter;

    winrt::com_ptr<ID3D12Device> d12_device;
    winrt::com_ptr<ID3D12CommandQueue> d12_queue;

    winrt::com_ptr<ID3D11Device> d11_device;
    winrt::com_ptr<ID3D11DeviceContext> d11_device_context;
};

Direct3DContext::Direct3DContext()
    : m_impl(make<Impl>())
{
}

Direct3DContext::~Direct3DContext() = default;

IDXGIAdapter1& Direct3DContext::adapter() const
{
    auto* adapter = m_impl->adapter.get();
    VERIFY(adapter != nullptr);
    return *adapter;
}

ID3D12Device& Direct3DContext::d12_device() const
{
    auto* d12_device = m_impl->d12_device.get();
    VERIFY(d12_device != nullptr);
    return *d12_device;
}

ID3D12CommandQueue& Direct3DContext::d12_command_queue() const
{
    auto* d12_queue = m_impl->d12_queue.get();
    VERIFY(d12_queue != nullptr);
    return *d12_queue;
}

ID3D11Device& Direct3DContext::d11_device() const
{
    auto* d11_device = m_impl->d11_device.get();
    VERIFY(d11_device != nullptr);
    return *d11_device;
}

ErrorOr<NonnullOwnPtr<Direct3DContext>> Direct3DContext::try_create()
{
    winrt::com_ptr<IDXGIFactory1> factory;
    if (HRESULT const hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.put())); FAILED(hr)) {
        return Error::from_windows_error(hr);
    }

    winrt::com_ptr<IDXGIAdapter1> adapter;
    winrt::com_ptr<IDXGIAdapter1> fallback_adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 adapter1_desc;
        if (HRESULT const hr = adapter->GetDesc1(&adapter1_desc); FAILED(hr)) {
            return Error::from_windows_error(hr);
        }
        if (adapter1_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE && fallback_adapter == nullptr) {
            fallback_adapter = adapter;
            adapter = nullptr;
        } else {
            break;
        }
    }

    if (adapter == nullptr && fallback_adapter != nullptr) {
        adapter = fallback_adapter;
    }

    if (adapter == nullptr) {
        return Error::from_string_literal("Unable to retrieve adapter");
    }

    winrt::com_ptr<ID3D12Device> d12_device;
    if (HRESULT const hr = D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(d12_device.put())); FAILED(hr)) {
        return Error::from_windows_error(hr);
    }

    winrt::com_ptr<ID3D12CommandQueue> d12_queue;
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (HRESULT const hr = d12_device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(d12_queue.put())); FAILED(hr)) {
        return Error::from_windows_error(hr);
    }

    winrt::com_ptr<ID3D11Device> d11_device;
    winrt::com_ptr<ID3D11DeviceContext> d11_device_context;
    UINT creation_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL feature_levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    if (HRESULT const hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, creation_flags, feature_levels, ARRAYSIZE(feature_levels), D3D11_SDK_VERSION, d11_device.put(), nullptr, d11_device_context.put()); FAILED(hr)) {
        return Error::from_windows_error(hr);
    }

    auto direct3d_context = adopt_own(*new Direct3DContext());
    direct3d_context->m_impl->factory = move(factory);
    direct3d_context->m_impl->adapter = move(adapter);
    direct3d_context->m_impl->d12_device = move(d12_device);
    direct3d_context->m_impl->d12_queue = move(d12_queue);
    direct3d_context->m_impl->d11_device = move(d11_device);
    direct3d_context->m_impl->d11_device_context = move(d11_device_context);
    return direct3d_context;
}

}
