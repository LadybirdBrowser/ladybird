/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibGfx/YUVData.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibMediaCodecs/VideoSurface.h>

#include "VideoFramePool.h"

namespace Media {

VideoFrameStorage::VideoFrameStorage(VideoFramePixels pixels, u64 sender_id, Function<void()> on_release)
    : m_pixels(move(pixels))
    , m_sender_id(sender_id)
    , m_on_release(move(on_release))
{
}

VideoFrameStorage::~VideoFrameStorage()
{
    if (m_on_release)
        m_on_release();
}

Core::AnonymousBuffer VideoFrameStorage::pixel_buffer() const
{
    if (auto const* buffer = m_pixels.get_pointer<Core::AnonymousBuffer>())
        return *buffer;
    return {};
}

RefPtr<VideoSurface> VideoFrameStorage::surface() const
{
    if (auto const* surface = m_pixels.get_pointer<NonnullRefPtr<VideoSurface>>())
        return *surface;
    return nullptr;
}

ErrorOr<NonnullRefPtr<VideoFramePool>> VideoFramePool::create(size_t byte_budget)
{
    return adopt_nonnull_ref_or_enomem(new (nothrow) VideoFramePool(byte_budget));
}

VideoFramePool::VideoFramePool(size_t byte_budget)
    : m_byte_budget(byte_budget)
{
}

VideoFramePool::~VideoFramePool() = default;

void VideoFramePool::set_slot_freed_callback(Function<void()> slot_freed_callback)
{
    MutexLocker locker { m_mutex };
    VERIFY(m_slots.is_empty());
    m_slot_freed_callback = move(slot_freed_callback);
}

Optional<VideoFramePool::AcquiredSlot> VideoFramePool::try_acquire(size_t byte_count)
{
    VERIFY(byte_count > 0);

    MutexLocker locker { m_mutex };

    Optional<u32> reusable_slot_index;
    Optional<u32> free_slot_index;
    for (u32 index = 0; index < m_slots.size(); index++) {
        auto const& slot = m_slots[index];
        if (slot.is_held)
            continue;
        if (!free_slot_index.has_value())
            free_slot_index = index;
        if (slot.buffer.is_valid() && slot.buffer.size() == byte_count) {
            reusable_slot_index = index;
            break;
        }
    }

    Slot* chosen_slot = nullptr;
    if (reusable_slot_index.has_value()) {
        chosen_slot = &m_slots[*reusable_slot_index];
    } else {
        // Reallocate a free slot to the requested capacity, growing the pool if every slot is held.
        if (!free_slot_index.has_value()) {
            if (m_slots.size() >= MAX_SLOT_COUNT)
                return {};
            if (m_slots.try_append({}).is_error())
                return {};
            free_slot_index = static_cast<u32>(m_slots.size() - 1);
        }
        chosen_slot = &m_slots[*free_slot_index];

        auto allocated_bytes_after = m_allocated_bytes - chosen_slot->buffer.size() + byte_count;
        if (allocated_bytes_after > m_byte_budget) {
            if (held_slot_count_while_locked() >= MIN_SLOT_COUNT) {
                dbgln_if(VIDEO_FRAME_POOL_DEBUG, "VideoFramePool: Refusing to allocate {} bytes over the budget of {}", byte_count, m_byte_budget);
                return {};
            }
            dbgln_if(VIDEO_FRAME_POOL_DEBUG, "VideoFramePool: Exceeding the byte budget ({} > {}) to keep the minimum {} slots live", allocated_bytes_after, m_byte_budget, MIN_SLOT_COUNT);
        }

        auto buffer_result = Core::AnonymousBuffer::create_with_size(byte_count);
        if (buffer_result.is_error())
            return {};
        drop_slot_storage_while_locked(*chosen_slot);
        chosen_slot->buffer = buffer_result.release_value();
        m_allocated_bytes += chosen_slot->buffer.size();
    }

    m_shed_storage_on_release = false;
    chosen_slot->is_held = true;

    free_excess_buffers_while_locked();

    auto slot_index = static_cast<u32>(chosen_slot - m_slots.data());
    auto storage = make_ref_counted<VideoFrameStorage>(chosen_slot->buffer, 0, [pool = NonnullRefPtr { *this }, slot_index] {
        pool->release_slot(slot_index);
    });
    return AcquiredSlot {
        .storage = move(storage),
        .bytes = { chosen_slot->buffer.data<u8>(), byte_count },
    };
}

u32 VideoFramePool::held_slot_count_while_locked() const
{
    u32 held_count = 0;
    for (auto const& slot : m_slots)
        held_count += slot.is_held;
    return held_count;
}

void VideoFramePool::drop_slot_storage_while_locked(Slot& slot)
{
    if (!slot.buffer.is_valid())
        return;
    m_allocated_bytes -= slot.buffer.size();
    slot.buffer = {};
}

void VideoFramePool::free_excess_buffers_while_locked()
{
    while (m_allocated_bytes > m_byte_budget) {
        Slot* largest_free_slot = nullptr;
        for (auto& slot : m_slots) {
            if (slot.is_held || !slot.buffer.is_valid())
                continue;
            if (largest_free_slot == nullptr || slot.buffer.size() > largest_free_slot->buffer.size())
                largest_free_slot = &slot;
        }
        if (largest_free_slot == nullptr)
            return;
        drop_slot_storage_while_locked(*largest_free_slot);
    }
}

void VideoFramePool::shed_storage()
{
    MutexLocker locker { m_mutex };
    m_shed_storage_on_release = true;
    for (auto& slot : m_slots) {
        if (!slot.is_held)
            drop_slot_storage_while_locked(slot);
    }
}

void VideoFramePool::release_slot(u32 slot_index)
{
    {
        MutexLocker locker { m_mutex };
        auto& slot = m_slots[slot_index];
        VERIFY(slot.is_held);
        slot.is_held = false;
        if (m_shed_storage_on_release)
            drop_slot_storage_while_locked(slot);
    }

    if (m_slot_freed_callback)
        m_slot_freed_callback();
}

size_t VideoFramePool::allocated_byte_count() const
{
    MutexLocker locker { m_mutex };
    return m_allocated_bytes;
}

VideoSurfaceBudget::~VideoSurfaceBudget() = default;

void VideoSurfaceBudget::set_storage_freed_callback(Function<void()> storage_freed_callback)
{
    MutexLocker locker { m_mutex };
    VERIFY(m_held_surface_count == 0);
    m_storage_freed_callback = move(storage_freed_callback);
}

RefPtr<VideoFrameStorage> VideoSurfaceBudget::try_hold(NonnullRefPtr<VideoSurface> surface, NonnullRefPtr<VideoSurfaceContents> contents)
{
    {
        MutexLocker locker { m_mutex };
        if (m_held_surface_count >= MAX_HELD_SURFACE_COUNT)
            return nullptr;
        m_held_surface_count++;
    }
    // The contents go before the decoder hears that the surface is free, so that it finds the surface free to decode
    // into again by the time it looks.
    return make_ref_counted<VideoFrameStorage>(move(surface), 0, [budget = NonnullRefPtr { *this }, contents = RefPtr<VideoSurfaceContents> { move(contents) }]() mutable {
        contents = nullptr;
        budget->release_hold();
    });
}

void VideoSurfaceBudget::release_hold()
{
    {
        MutexLocker locker { m_mutex };
        VERIFY(m_held_surface_count > 0);
        m_held_surface_count--;
    }
    if (m_storage_freed_callback)
        m_storage_freed_callback();
}

ErrorOr<Gfx::YUVData> yuv_data_in_buffer(Core::AnonymousBuffer const& buffer, Gfx::IntSize size, u8 bit_depth, Subsampling subsampling, CodingIndependentCodePoints const& cicp)
{
    if (!buffer.is_valid() || buffer.size() == 0)
        return Error::from_string_literal("Invalid video frame buffer");

    auto layout = TRY(frame_plane_layout(size, bit_depth, subsampling));
    if (layout.total_byte_count > buffer.size())
        return Error::from_string_literal("Video frame format does not fit its buffer");

    auto bytes = Bytes { const_cast<u8*>(buffer.data<u8 const>()), buffer.size() };
    return Gfx::YUVData::create(size, bit_depth, subsampling, cicp, bytes.slice(0, layout.y_size), bytes.slice(layout.u_offset, layout.u_size), bytes.slice(layout.v_offset, layout.v_size));
}

ErrorOr<NonnullRefPtr<VideoFrame>> create_received_video_frame(VideoFrameMetadata const& metadata, VideoFramePixels pixels, u64 sender_id, Function<void()> on_release)
{
    auto storage = make_ref_counted<VideoFrameStorage>(move(pixels), sender_id, move(on_release));

    if (!metadata.is_valid())
        return Error::from_string_literal("Video frame metadata describes a frame that could not have been decoded");

    if (auto surface = storage->surface()) {
        auto surface_size = surface->size();
        if (metadata.subsampling != Subsampling::yuv420() || metadata.size.width() > surface_size.width() || metadata.size.height() > surface_size.height())
            return Error::from_string_literal("Video frame metadata does not match its surface");
    } else {
        TRY(yuv_data_in_buffer(storage->pixel_buffer(), metadata.size, metadata.bit_depth, metadata.subsampling, metadata.cicp));
    }

    return make_ref_counted<VideoFrame>(metadata.timestamp, metadata.duration, metadata.size.to_type<u32>(), metadata.bit_depth, metadata.subsampling, metadata.cicp, move(storage));
}

}
