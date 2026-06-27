/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Checked.h>
#include <AK/Debug.h>
#include <LibGfx/YUVData.h>
#include <LibMedia/VideoFrame.h>

#include "VideoFramePool.h"

namespace Media {

namespace {

constexpr size_t SLOT_DATA_ALIGNMENT = 64;

VideoFramePoolID allocate_pool_id()
{
    static Atomic<u64> s_next_id { 1 };
    return VideoFramePoolID { s_next_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed) };
}

struct SlotHeader {
    Atomic<u64, AK::MemoryOrder::memory_order_relaxed> slot_acquisition_id { 0 };
};

constexpr size_t slot_data_offset()
{
    return align_up_to(sizeof(SlotHeader), SLOT_DATA_ALIGNMENT);
}

SlotHeader& slot_header(Core::AnonymousBuffer& buffer)
{
    return *reinterpret_cast<SlotHeader*>(buffer.data<u8>());
}

SlotHeader const& slot_header(Core::AnonymousBuffer const& buffer)
{
    return *reinterpret_cast<SlotHeader const*>(buffer.data<u8 const>());
}

}

ErrorOr<NonnullRefPtr<VideoFramePool>> VideoFramePool::create(Function<void()> slot_freed_callback, size_t byte_budget)
{
    return adopt_nonnull_ref_or_enomem(new (nothrow) VideoFramePool(move(slot_freed_callback), byte_budget));
}

VideoFramePool::VideoFramePool(Function<void()> slot_freed_callback, size_t byte_budget)
    : m_id(allocate_pool_id())
    , m_slot_freed_callback(move(slot_freed_callback))
    , m_byte_budget(byte_budget)
{
}

Optional<VideoFramePool::AcquiredSlot> VideoFramePool::try_acquire(size_t byte_count)
{
    VERIFY(byte_count > 0);

    Sync::MutexLocker locker { m_mutex };

    m_shed_buffers_on_release = false;

    // Reuse a free slot with matching capacity.
    Slot* chosen_slot = nullptr;
    for (auto& slot : m_slots) {
        if (slot.hold_count != 0 || !slot.buffer.is_valid())
            continue;
        if (slot.buffer.size() - slot_data_offset() != byte_count)
            continue;
        chosen_slot = &slot;
        break;
    }

    if (chosen_slot == nullptr) {
        // Reallocate the first free slot to hold the requested capacity.
        for (auto& slot : m_slots) {
            if (slot.hold_count == 0) {
                chosen_slot = &slot;
                break;
            }
        }
        if (chosen_slot == nullptr)
            return {};

        Checked<size_t> checked_buffer_size = byte_count;
        checked_buffer_size += slot_data_offset();
        if (checked_buffer_size.has_overflow())
            return {};
        auto buffer_size = checked_buffer_size.value();

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
        drop_slot_buffer_while_locked(*chosen_slot);
        chosen_slot->buffer = buffer_result.release_value();
        chosen_slot->allocated_buffer_id++;
        new (chosen_slot->buffer.data<void>()) SlotHeader;
        m_allocated_bytes += chosen_slot->buffer.size();
    }

    chosen_slot->hold_count = 1;
    chosen_slot->last_slot_acquisition_id++;
    slot_header(chosen_slot->buffer).slot_acquisition_id.store(chosen_slot->last_slot_acquisition_id);
    AK::atomic_thread_fence(AK::MemoryOrder::memory_order_seq_cst);

    free_excess_buffers_while_locked();

    auto slot_index = static_cast<u32>(chosen_slot - m_slots.data());
    return AcquiredSlot {
        .index = slot_index,
        .slot_acquisition_id = chosen_slot->last_slot_acquisition_id,
        .allocated_buffer_id = chosen_slot->allocated_buffer_id,
        .bytes = { chosen_slot->buffer.data<u8>() + slot_data_offset(), byte_count },
    };
}

ErrorOr<NonnullRefPtr<PooledVideoFrameSlot>> VideoFramePool::try_adopt_acquired_slot(AcquiredSlot const& acquired_slot)
{
    auto slot_result = try_make_ref_counted<PooledVideoFrameSlot>(*this, acquired_slot.index, acquired_slot.slot_acquisition_id, acquired_slot.allocated_buffer_id);
    if (slot_result.is_error())
        release_hold(acquired_slot.index);
    return slot_result;
}

u32 VideoFramePool::held_slot_count_while_locked() const
{
    u32 held_count = 0;
    for (auto const& slot : m_slots)
        held_count += slot.hold_count != 0;
    return held_count;
}

void VideoFramePool::drop_slot_buffer_while_locked(Slot& slot)
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
        drop_slot_buffer_while_locked(*largest_free_slot);
    }
}

void VideoFramePool::add_hold(u32 slot_index)
{
    Sync::MutexLocker locker { m_mutex };
    VERIFY(m_slots[slot_index].hold_count > 0);
    m_slots[slot_index].hold_count++;
}

void VideoFramePool::release_hold(u32 slot_index)
{
    {
        Sync::MutexLocker locker { m_mutex };
        auto& slot = m_slots[slot_index];
        VERIFY(slot.hold_count > 0);
        if (--slot.hold_count > 0)
            return;
        if (m_shed_buffers_on_release)
            drop_slot_buffer_while_locked(slot);
    }

    if (m_slot_freed_callback)
        m_slot_freed_callback();
}

void VideoFramePool::shed_buffers()
{
    Sync::MutexLocker locker { m_mutex };
    m_shed_buffers_on_release = true;
    for (auto& slot : m_slots) {
        if (slot.hold_count == 0)
            drop_slot_buffer_while_locked(slot);
    }
}

Core::AnonymousBuffer VideoFramePool::slot_buffer(u32 slot_index) const
{
    Sync::MutexLocker locker { m_mutex };
    VERIFY(m_slots[slot_index].hold_count > 0);
    return m_slots[slot_index].buffer;
}

size_t VideoFramePool::allocated_byte_count() const
{
    Sync::MutexLocker locker { m_mutex };
    return m_allocated_bytes;
}

bool ResolvedVideoFrameSlot::revalidate() const
{
    AK::atomic_thread_fence(AK::MemoryOrder::memory_order_acquire);
    return slot_header(m_slot_buffer).slot_acquisition_id.load() == m_slot_acquisition_id;
}

ErrorOr<NonnullRefPtr<VideoFrame>> resolve_frame_from_slot_buffer(Core::AnonymousBuffer const& slot_buffer, VideoFrameHandle const& handle, Function<void()> on_release)
{
    if (!slot_buffer.is_valid() || slot_buffer.size() <= slot_data_offset())
        return Error::from_string_literal("Invalid video frame slot buffer");

    auto layout = TRY(frame_plane_layout(handle.size, handle.bit_depth, handle.subsampling));
    auto capacity = slot_buffer.size() - slot_data_offset();
    if (layout.total_byte_count > capacity)
        return Error::from_string_literal("VideoFrameHandle format does not fit its slot");

    if (slot_header(slot_buffer).slot_acquisition_id.load(AK::MemoryOrder::memory_order_acquire) != handle.slot_acquisition_id)
        return Error::from_string_literal("VideoFrameHandle refers to a recycled slot");

    auto bytes = Bytes { const_cast<u8*>(slot_buffer.data<u8 const>()) + slot_data_offset(), capacity };
    auto y_data = bytes.slice(0, layout.y_size);
    auto u_data = bytes.slice(layout.u_offset, layout.u_size);
    auto v_data = bytes.slice(layout.v_offset, layout.v_size);
    auto yuv_data = TRY(Gfx::YUVData::create(handle.size, handle.bit_depth, handle.subsampling, handle.cicp, y_data, u_data, v_data));

    auto resolved_slot = TRY(try_make_ref_counted<ResolvedVideoFrameSlot>(slot_buffer, handle.pool_id, handle.slot_index, handle.slot_acquisition_id, move(on_release)));
    return try_make_ref_counted<VideoFrame>(handle.timestamp, handle.duration, handle.size.to_type<u32>(), handle.bit_depth, yuv_data, move(resolved_slot));
}

NonnullRefPtr<VideoFrameSlotDirectory> VideoFrameSlotDirectory::create()
{
    return adopt_ref(*new VideoFrameSlotDirectory());
}

void VideoFrameSlotDirectory::set_on_slots_changed(Function<void()> on_slots_changed)
{
    m_on_slots_changed = move(on_slots_changed);
}

void VideoFrameSlotDirectory::notify_slot_announced(VideoFramePoolID pool_id, u32 slot_index, Core::AnonymousBuffer slot_buffer)
{
    if (!slot_buffer.is_valid() || slot_buffer.size() <= slot_data_offset())
        return;
    m_slot_buffers_by_pool_id.ensure(pool_id).set(slot_index, move(slot_buffer));
    if (m_on_slots_changed)
        m_on_slots_changed();
}

void VideoFrameSlotDirectory::notify_pool_retired(VideoFramePoolID pool_id)
{
    m_slot_buffers_by_pool_id.remove(pool_id);
}

RefPtr<VideoFrame> VideoFrameSlotDirectory::resolve_frame(VideoFrameHandle const& handle, Function<void()> on_release) const
{
    auto slot_buffers = m_slot_buffers_by_pool_id.get(handle.pool_id);
    if (!slot_buffers.has_value())
        return nullptr;
    auto slot_buffer = slot_buffers->get(handle.slot_index);
    if (!slot_buffer.has_value())
        return nullptr;

    auto frame_or_error = resolve_frame_from_slot_buffer(*slot_buffer, handle, move(on_release));
    if (frame_or_error.is_error())
        return nullptr;
    return frame_or_error.release_value();
}

}
