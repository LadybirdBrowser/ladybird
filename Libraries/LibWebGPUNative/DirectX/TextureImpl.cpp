/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <AK/Span.h>
#include <LibWebGPUNative/DirectX/DeviceImpl.h>
#include <LibWebGPUNative/DirectX/Error.h>
#include <LibWebGPUNative/DirectX/TextureImpl.h>

namespace WebGPUNative {

Texture::Impl::Impl(Device const& gpu_device, Gfx::IntSize const size)
    : m_size(size)
    , m_device(gpu_device.m_impl->device())
    , m_command_queue(gpu_device.m_impl->command_queue())
{
}

ErrorOr<void> Texture::Impl::initialize()
{
    // FIXME: Don't hardcode these settings
    D3D12_RESOURCE_DESC texture_desc = {};
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.Width = static_cast<UINT64>(m_size.width());
    texture_desc.Height = static_cast<UINT64>(m_size.height());
    texture_desc.DepthOrArraySize = 1;
    texture_desc.MipLevels = 1;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES texture_heap_props = {};
    texture_heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    if (FAILED(m_device->CreateCommittedResource(&texture_heap_props, D3D12_HEAP_FLAG_NONE, &texture_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&m_texture))))
        return make_error("Unable to create texture resource");

    return {};
}

ErrorOr<NonnullOwnPtr<MappedTextureBuffer>> Texture::Impl::map_buffer()
{
    D3D12_RESOURCE_DESC texture_desc = m_texture->GetDesc();
    UINT64 buffer_size;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    m_device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &buffer_size);

    D3D12_RESOURCE_DESC drawing_buffer_desc = {};
    drawing_buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    drawing_buffer_desc.Width = buffer_size;
    drawing_buffer_desc.Height = 1;
    drawing_buffer_desc.DepthOrArraySize = 1;
    drawing_buffer_desc.MipLevels = 1;
    drawing_buffer_desc.Format = DXGI_FORMAT_UNKNOWN;
    drawing_buffer_desc.SampleDesc.Count = 1;
    drawing_buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES drawing_buffer_heap_props = {};
    drawing_buffer_heap_props.Type = D3D12_HEAP_TYPE_READBACK;

    if (FAILED(m_device->CreateCommittedResource(&drawing_buffer_heap_props, D3D12_HEAP_FLAG_NONE, &drawing_buffer_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_drawing_buffer))))
        return make_error("Unable to create read back drawing buffer resource");

    if (HRESULT const result = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_command_allocator)); FAILED(result))
        return make_error(result, "Unable to create command allocator");

    if (HRESULT const result = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_command_allocator.Get(), nullptr, IID_PPV_ARGS(&m_command_list)); FAILED(result))
        return make_error(result, "Unable to create command list");

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_command_list->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION source_location = {};
    source_location.pResource = m_texture.Get();
    source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source_location.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION destination_location = {};
    destination_location.pResource = m_drawing_buffer.Get();
    destination_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination_location.PlacedFootprint = footprint;

    m_command_list->CopyTextureRegion(&destination_location, 0, 0, 0, &source_location, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_command_list->ResourceBarrier(1, &barrier);

    m_command_list->Close();
    ID3D12CommandList* command_lists[] = { m_command_list.Get() };
    m_command_queue->ExecuteCommandLists(1, command_lists);

    ComPtr<ID3D12Fence> fence;
    if (HRESULT const result = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)); FAILED(result))
        return make_error(result, "Unable to create fence");

    // FIXME: Queue submission should be asynchronous
    HANDLE fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event)
        return make_error("Unable to create fence event for command buffer submission");
    m_command_queue->Signal(fence.Get(), 1);

    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, fence_event);
        WaitForSingleObject(fence_event, INFINITE);
    }

    u8* mapped_buffer = nullptr;
    D3D12_RANGE drawing_buffer_read_range = { 0, buffer_size };
    if (HRESULT const result = m_drawing_buffer->Map(0, &drawing_buffer_read_range, reinterpret_cast<void**>(&mapped_buffer)); FAILED(result))
        return make_error(result, "Unable to map drawing buffer resource");

    // NOTE: ErrorOr disable RVO, so we use reference semantics here to avoid the destructive move that results in unmapping the buffer twice
    return make<MappedTextureBuffer>(*this, mapped_buffer, buffer_size, footprint.Footprint.RowPitch);
}

void Texture::Impl::unmap_buffer()
{
    D3D12_RANGE drawing_buffer_write_range = { 0, 0 };
    m_drawing_buffer->Unmap(0, &drawing_buffer_write_range);
}

MappedTextureBuffer::MappedTextureBuffer(Texture::Impl& texture_impl, u8* buffer, size_t buffer_size, u32 row_pitch)
    : m_texture_impl(texture_impl)
    , m_buffer(buffer, buffer_size)
    , m_row_pitch(row_pitch)
{
}

MappedTextureBuffer::~MappedTextureBuffer()
{
    m_texture_impl->unmap_buffer();
}

int MappedTextureBuffer::width() const
{
    return m_texture_impl->size().width();
}

int MappedTextureBuffer::height() const
{
    return m_texture_impl->size().height();
}

}
