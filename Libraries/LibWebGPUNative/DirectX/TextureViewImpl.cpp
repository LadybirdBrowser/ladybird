/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/DirectX/DeviceImpl.h>
#include <LibWebGPUNative/DirectX/Error.h>
#include <LibWebGPUNative/DirectX/TextureImpl.h>
#include <LibWebGPUNative/DirectX/TextureViewImpl.h>

namespace WebGPUNative {

TextureView::Impl::Impl(Texture const& gpu_texture)
    : m_size(gpu_texture.size())
    , m_device(gpu_texture.m_impl->device())
    , m_texture(gpu_texture.m_impl->texture())
{
}

ErrorOr<void> TextureView::Impl::initialize()
{
    D3D12_DESCRIPTOR_HEAP_DESC texture_view_heap_description = {};
    texture_view_heap_description.NumDescriptors = 1;
    texture_view_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;

    if (HRESULT const result = m_device->CreateDescriptorHeap(&texture_view_heap_description, IID_PPV_ARGS(&m_texture_view_heap)); FAILED(result))
        return make_error(result, "Unable to create texture view descriptor heap");

    m_texture_view_handle = m_texture_view_heap->GetCPUDescriptorHandleForHeapStart();
    m_device->CreateRenderTargetView(m_texture.Get(), nullptr, m_texture_view_handle);
    return {};
}

}
