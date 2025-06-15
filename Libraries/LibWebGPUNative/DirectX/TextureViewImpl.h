/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Size.h>
#include <LibWebGPUNative/TextureView.h>
#include <d3d12.h>
#include <wrl.h>

using namespace Microsoft::WRL;

namespace WebGPUNative {

struct TextureView::Impl {
    explicit Impl(Texture const& gpu_texture);

    ErrorOr<void> initialize();

    Gfx::IntSize size() const { return m_size; }

    D3D12_CPU_DESCRIPTOR_HANDLE texture_view_handle() const { return m_texture_view_handle; }

private:
    Gfx::IntSize m_size;
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12Resource> m_texture;
    ComPtr<ID3D12DescriptorHeap> m_texture_view_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_texture_view_handle;
};

}
