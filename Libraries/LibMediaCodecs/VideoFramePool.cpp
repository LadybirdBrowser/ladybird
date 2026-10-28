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

ErrorOr<NonnullRefPtr<VideoFramePool>> VideoFramePool::create(size_t byte_budget)
{
    return adopt_nonnull_ref_or_enomem(new (nothrow) VideoFramePool(byte_budget));
}

VideoFrameEntryLedger::~VideoFrameEntryLedger() = default;

void VideoFrameEntryLedger::set_slot_freed_callback(Function<void()> slot_freed_callback)
{
    MutexLocker locker { m_mutex };
    VERIFY(m_slots.is_empty());
    m_slot_freed_callback = move(slot_freed_callback);
}

VideoFramePool::VideoFramePool(size_t byte_budget)
    : m_byte_budget(byte_budget)
{
}

Optional<VideoFramePool::AcquiredSlot> VideoFramePool::try_acquire(size_t byte_count)
{
    VERIFY(byte_count > 0);

    MutexLocker locker { m_mutex };

    Optional<u32> reusable_slot_index;
    Optional<u32> free_slot_index;
    for (u32 index = 0; index < m_slots.size(); index++) {
        auto const& slot = m_slots[index];
        if (slot.hold_count != 0)
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
            free_slot_index = try_grow_while_locked();
            if (!free_slot_index.has_value())
                return {};
        }
        chosen_slot = &m_slots[*free_slot_index];

        auto buffer_size = byte_count;
        auto allocated_bytes_after = m_allocated_bytes - chosen_slot->buffer.size() + buffer_size;
        if (allocated_bytes_after > m_byte_budget) {
            if (held_slot_count_while_locked() >= MIN_SLOT_COUNT) {
                dbgln_if(VIDEO_FRAME_POOL_DEBUG, "VideoFramePool: Refusing to allocate {} bytes over the budget of {}", buffer_size, m_byte_budget);
                return {};
            }
            dbgln_if(VIDEO_FRAME_POOL_DEBUG, "VideoFramePool: Exceeding the byte budget ({} > {}) to keep the minimum {} slots live", allocated_bytes_after, m_byte_budget, MIN_SLOT_COUNT);
        }

        auto buffer_result = Core::AnonymousBuffer::create_with_size(buffer_size);
        if (buffer_result.is_error())
            return {};
        drop_slot_storage_while_locked(*chosen_slot);
        chosen_slot->buffer = buffer_result.release_value();
        m_allocated_bytes += chosen_slot->buffer.size();
    }

    begin_hold_while_locked(*chosen_slot);

    free_excess_buffers_while_locked();

    auto slot_index = static_cast<u32>(chosen_slot - m_slots.data());
    return AcquiredSlot {
        .index = slot_index,
        .bytes = { chosen_slot->buffer.data<u8>(), byte_count },
    };
}

ErrorOr<NonnullRefPtr<PooledVideoFrameSlot>> VideoFramePool::try_adopt_acquired_slot(AcquiredSlot const& acquired_slot)
{
    return try_adopt_slot(acquired_slot.index);
}

void VideoFrameEntryLedger::begin_hold_while_locked(Slot& slot)
{
    m_shed_storage_on_release = false;
    slot.hold_count = 1;
}

Optional<u32> VideoFrameEntryLedger::try_grow_while_locked()
{
    if (m_slots.try_append({}).is_error())
        return {};
    return static_cast<u32>(m_slots.size() - 1);
}

RefPtr<VideoSurface> VideoFrameEntryLedger::slot_surface(u32 slot_index) const
{
    MutexLocker locker { m_mutex };
    VERIFY(m_slots[slot_index].hold_count > 0);
    return m_slots[slot_index].surface;
}

ErrorOr<NonnullRefPtr<PooledVideoFrameSlot>> VideoFrameEntryLedger::try_adopt_slot(u32 slot_index)
{
    auto slot_result = try_make_ref_counted<PooledVideoFrameSlot>(*this, slot_index);
    if (slot_result.is_error())
        release_hold(slot_index);
    return slot_result;
}

u32 VideoFrameEntryLedger::held_slot_count_while_locked() const
{
    u32 held_count = 0;
    for (auto const& slot : m_slots)
        held_count += slot.hold_count != 0;
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
            if (slot.hold_count != 0 || !slot.buffer.is_valid())
                continue;
            if (largest_free_slot == nullptr || slot.buffer.size() > largest_free_slot->buffer.size())
                largest_free_slot = &slot;
        }
        if (largest_free_slot == nullptr)
            return;
        drop_slot_storage_while_locked(*largest_free_slot);
    }
}

void VideoFrameEntryLedger::shed_storage()
{
    MutexLocker locker { m_mutex };
    m_shed_storage_on_release = true;
    for (auto& slot : m_slots) {
        if (slot.hold_count == 0)
            drop_slot_storage_while_locked(slot);
    }
}

void VideoFrameEntryLedger::add_hold(u32 slot_index)
{
    MutexLocker locker { m_mutex };
    VERIFY(m_slots[slot_index].hold_count > 0);
    m_slots[slot_index].hold_count++;
}

void VideoFrameEntryLedger::release_hold(u32 slot_index)
{
    // The surface and its contents go outside the lock, and before the decoder hears that the slot is free, so that it
    // finds the surface free to decode into again by the time it looks.
    RefPtr<VideoSurface> released_surface;
    RefPtr<VideoSurfaceContents> released_contents;
    {
        MutexLocker locker { m_mutex };
        auto& slot = m_slots[slot_index];
        VERIFY(slot.hold_count > 0);
        if (--slot.hold_count > 0)
            return;
        released_surface = move(slot.surface);
        released_contents = move(slot.contents);
        if (m_shed_storage_on_release)
            drop_slot_storage_while_locked(slot);
    }
    released_contents = nullptr;
    released_surface = nullptr;

    if (m_slot_freed_callback)
        m_slot_freed_callback();
}

Core::AnonymousBuffer VideoFrameEntryLedger::slot_buffer(u32 slot_index) const
{
    MutexLocker locker { m_mutex };
    VERIFY(m_slots[slot_index].hold_count > 0);
    return m_slots[slot_index].buffer;
}

size_t VideoFramePool::allocated_byte_count() const
{
    MutexLocker locker { m_mutex };
    return m_allocated_bytes;
}

ErrorOr<NonnullRefPtr<VideoFrameSurfacePool>> VideoFrameSurfacePool::create()
{
    return adopt_nonnull_ref_or_enomem(new (nothrow) VideoFrameSurfacePool());
}

Optional<VideoFrameSurfacePool::AcquiredSlot> VideoFrameSurfacePool::try_acquire(NonnullRefPtr<VideoSurface> const& surface, NonnullRefPtr<VideoSurfaceContents> contents)
{
    MutexLocker locker { m_mutex };

    Optional<u32> free_slot_index;
    for (u32 index = 0; index < m_slots.size(); index++) {
        if (m_slots[index].hold_count == 0) {
            free_slot_index = index;
            break;
        }
    }
    if (!free_slot_index.has_value()) {
        if (m_slots.size() >= MAX_HELD_SLOT_COUNT)
            return {};
        free_slot_index = try_grow_while_locked();
        if (!free_slot_index.has_value())
            return {};
    }

    auto& slot = m_slots[free_slot_index.value()];
    slot.surface = surface;
    slot.contents = move(contents);
    begin_hold_while_locked(slot);
    return AcquiredSlot { .index = free_slot_index.value() };
}

ErrorOr<NonnullRefPtr<PooledVideoFrameSlot>> VideoFrameSurfacePool::try_adopt_acquired_slot(AcquiredSlot const& acquired_slot)
{
    return try_adopt_slot(acquired_slot.index);
}

ReceivedVideoFrameStorage::ReceivedVideoFrameStorage(Core::AnonymousBuffer pixel_buffer, RefPtr<VideoSurface> surface, u64 sender_id, Function<void()> on_release)
    : m_pixel_buffer(move(pixel_buffer))
    , m_surface(move(surface))
    , m_sender_id(sender_id)
    , m_on_release(move(on_release))
{
}

ReceivedVideoFrameStorage::~ReceivedVideoFrameStorage()
{
    if (m_on_release)
        m_on_release();
}

ErrorOr<Gfx::YUVData> yuv_data_in_slot_buffer(Core::AnonymousBuffer const& slot_buffer, Gfx::IntSize size, u8 bit_depth, Subsampling subsampling, CodingIndependentCodePoints const& cicp)
{
    if (!slot_buffer.is_valid() || slot_buffer.size() == 0)
        return Error::from_string_literal("Invalid video frame buffer");

    auto layout = TRY(frame_plane_layout(size, bit_depth, subsampling));
    if (layout.total_byte_count > slot_buffer.size())
        return Error::from_string_literal("Video frame format does not fit its buffer");

    auto bytes = Bytes { const_cast<u8*>(slot_buffer.data<u8 const>()), slot_buffer.size() };
    return Gfx::YUVData::create(size, bit_depth, subsampling, cicp, bytes.slice(0, layout.y_size), bytes.slice(layout.u_offset, layout.u_size), bytes.slice(layout.v_offset, layout.v_size));
}

ErrorOr<NonnullRefPtr<VideoFrame>> create_received_video_frame(VideoFrameMetadata const& metadata, Core::AnonymousBuffer pixel_buffer, RefPtr<VideoSurface> surface, u64 sender_id, Function<void()> on_release)
{
    auto storage_or_error = try_make_ref_counted<ReceivedVideoFrameStorage>(move(pixel_buffer), move(surface), sender_id, move(on_release));
    if (storage_or_error.is_error()) {
        if (on_release)
            on_release();
        return storage_or_error.release_error();
    }
    auto storage = storage_or_error.release_value();

    if (!metadata.is_valid())
        return Error::from_string_literal("Video frame metadata describes a frame that could not have been decoded");

    if (auto const& received_surface = storage->surface()) {
        if (storage->pixel_buffer().is_valid())
            return Error::from_string_literal("Video frame was sent with both a surface and a pixel buffer");
        auto surface_size = received_surface->size();
        if (metadata.subsampling != Subsampling::yuv420() || metadata.size.width() > surface_size.width() || metadata.size.height() > surface_size.height())
            return Error::from_string_literal("Video frame metadata does not match its surface");
    } else {
        TRY(yuv_data_in_slot_buffer(storage->pixel_buffer(), metadata.size, metadata.bit_depth, metadata.subsampling, metadata.cicp));
    }

    return try_make_ref_counted<VideoFrame>(metadata.timestamp, metadata.duration, metadata.size.to_type<u32>(), metadata.bit_depth, metadata.subsampling, metadata.cicp, move(storage));
}

}
