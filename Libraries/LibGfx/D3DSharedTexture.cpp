/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/D3DSharedTexture.h>
#include <LibGfx/Direct3DContext.h>

#ifdef USE_DIRECTX

#    include <AK/Windows.h>
#    include <d3d12.h>

#    include <wrl/client.h>

namespace Gfx {

using Microsoft::WRL::ComPtr;

D3DSharedTexture::D3DSharedTexture(ID3D12Resource* resource, IPC::File shared_handle, IntSize size)
    : m_resource(resource)
    , m_shared_handle(move(shared_handle))
    , m_size(size)
{
}

D3DSharedTexture::~D3DSharedTexture()
{
    m_resource->Release();
}

ErrorOr<NonnullRefPtr<D3DSharedTexture>> D3DSharedTexture::create(Direct3DContext& context, IntSize size)
{
    VERIFY(!size.is_empty());

    D3D12_HEAP_PROPERTIES heap_properties {};
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC resource_description {};
    resource_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_description.Width = size.width();
    resource_description.Height = size.height();
    resource_description.DepthOrArraySize = 1;
    resource_description.MipLevels = 1;
    resource_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    resource_description.SampleDesc.Count = 1;
    resource_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // ALLOW_SIMULTANEOUS_ACCESS is required for the UI process to open this resource on a D3D11 device.
    resource_description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    ComPtr<ID3D12Resource> resource;
    auto hr = context.device()->CreateCommittedResource(&heap_properties, D3D12_HEAP_FLAG_SHARED, &resource_description,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource));
    if (FAILED(hr))
        return Error::from_windows_error(hr);

    HANDLE shared_handle = nullptr;
    hr = context.device()->CreateSharedHandle(resource.Get(), nullptr, GENERIC_ALL, nullptr, &shared_handle);
    if (FAILED(hr))
        return Error::from_windows_error(hr);

    return adopt_ref(*new D3DSharedTexture(resource.Detach(), IPC::File::adopt_fd(to_fd(shared_handle)), size));
}

}

#endif
