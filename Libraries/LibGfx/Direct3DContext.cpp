/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/NonnullRefPtr.h>
#include <LibGfx/Direct3DContext.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl.h>

using namespace Microsoft::WRL;

namespace Gfx {

struct Direct3D11Texture::Impl {
    Direct3DContext const& context;
    ComPtr<ID3D11Texture2D> d11_texture;
    ComPtr<IDXGIResource> dxgi_resource;
};

Direct3D11Texture::Direct3D11Texture(Direct3DContext const& context)
    : m_impl(make<Impl>(context))
{
}

Direct3D11Texture::~Direct3D11Texture() = default;

IDXGIResource& Direct3D11Texture::get_resource() const
{
    auto* resource = m_impl->dxgi_resource.Get();
    VERIFY(resource != nullptr);
    return *resource;
}

ErrorOr<NonnullRefPtr<Direct3D11Texture>> Direct3D11Texture::try_create_shared(Direct3DContext const& context, u32 const width, u32 const height, DXGI_FORMAT const format)
{
    D3D11_TEXTURE2D_DESC tex_desc = {};
    tex_desc.Width = width;
    tex_desc.Height = height;
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = format;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.SampleDesc.Quality = 0;
    tex_desc.Usage = D3D11_USAGE_DEFAULT;
    tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    tex_desc.CPUAccessFlags = 0;
    tex_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    NonnullRefPtr<Direct3D11Texture> texture = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) Direct3D11Texture(context)));
    if (HRESULT const hr = context.d11_device().CreateTexture2D(&tex_desc, nullptr, &texture->m_impl->d11_texture); FAILED(hr)) {
        return Error::from_windows_error(hr);
    }
    if (HRESULT const hr = texture->m_impl->d11_texture.As(&texture->m_impl->dxgi_resource); FAILED(hr)) {
        return Error::from_windows_error(hr);
    }
    return texture;
}

struct Direct3DContext::Impl {
    ComPtr<IDXGIFactory1> factory;
    ComPtr<IDXGIAdapter1> adapter;

    ComPtr<ID3D12Device> d12_device;
    ComPtr<ID3D12CommandQueue> d12_queue;

    ComPtr<ID3D11Device> d11_device;
    ComPtr<ID3D11DeviceContext> d11_device_context;
};

Direct3DContext::Direct3DContext()
    : m_impl(make<Impl>())
{
}

Direct3DContext::~Direct3DContext() = default;

IDXGIAdapter1& Direct3DContext::adapter() const
{
    auto* adapter = m_impl->adapter.Get();
    VERIFY(adapter != nullptr);
    return *adapter;
}

ID3D12Device& Direct3DContext::d12_device() const
{
    auto* d12_device = m_impl->d12_device.Get();
    VERIFY(d12_device != nullptr);
    return *d12_device;
}

ID3D12CommandQueue& Direct3DContext::d12_command_queue() const
{
    auto* d12_queue = m_impl->d12_queue.Get();
    VERIFY(d12_queue != nullptr);
    return *d12_queue;
}

ID3D11Device& Direct3DContext::d11_device() const
{
    auto* d11_device = m_impl->d11_device.Get();
    VERIFY(d11_device != nullptr);
    return *d11_device;
}

ErrorOr<NonnullOwnPtr<Direct3DContext>> Direct3DContext::try_create()
{
    ComPtr<IDXGIFactory1> factory;
    if (HRESULT const hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory)); FAILED(hr)) {
        return Error::from_windows_error(hr);
    }

    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIAdapter1> fallback_adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 adapter1_desc;
        if (HRESULT const hr = adapter->GetDesc1(&adapter1_desc); FAILED(hr)) {
            return Error::from_windows_error(hr);
        }
        if (adapter1_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE && fallback_adapter == nullptr) {
            fallback_adapter = adapter;
            adapter.Reset();
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

    ComPtr<ID3D12Device> d12_device;
    if (HRESULT const hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d12_device));
        FAILED(hr)) {
        return Error::from_windows_error(hr);
    }

    ComPtr<ID3D12CommandQueue> d12_queue;
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (HRESULT const hr = d12_device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&d12_queue)); FAILED(hr)) {
        return Error::from_windows_error(hr);
    }

    ComPtr<ID3D11Device> d11_device;
    ComPtr<ID3D11DeviceContext> d11_device_context;
    UINT creation_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL feature_levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    if (HRESULT const hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, creation_flags, feature_levels, ARRAYSIZE(feature_levels), D3D11_SDK_VERSION, &d11_device, nullptr, &d11_device_context); FAILED(hr)) {
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
