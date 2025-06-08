/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebGPUNative/Texture.h>
#include <d3d12.h>
#include <wrl.h>

using namespace Microsoft::WRL;

namespace WebGPUNative {

struct Texture::Impl {
    explicit Impl(Device const& gpu_device, Gfx::IntSize size);

    ErrorOr<void> initialize();

    Gfx::IntSize size() const { return m_size; }

    ComPtr<ID3D12Device> device() const { return m_device; }
    ComPtr<ID3D12Resource> texture() const { return m_texture; }

    ErrorOr<NonnullOwnPtr<MappedTextureBuffer>> map_buffer();
    void unmap_buffer();

private:
    Gfx::IntSize m_size;
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_command_queue;
    ComPtr<ID3D12CommandAllocator> m_command_allocator;
    ComPtr<ID3D12GraphicsCommandList> m_command_list;
    ComPtr<ID3D12Resource> m_texture;
    ComPtr<ID3D12Resource> m_drawing_buffer;
};

}
