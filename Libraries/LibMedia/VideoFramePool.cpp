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
#include <LibMedia/VideoSurface.h>

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

ErrorOr<NonnullRefPtr<VideoFramePool>> VideoFramePool::create(size_t byte_budget)
{
    return adopt_nonnull_ref_or_enomem(new (nothrow) VideoFramePool(byte_budget));
}

VideoFrameEntryLedger::VideoFrameEntryLedger()
    : m_id(allocate_pool_id())
{
}

VideoFrameEntryLedger::~VideoFrameEntryLedger() = default;

void VideoFrameEntryLedger::set_slot_freed_callback(Function<void()> slot_freed_callback)
{
    Sync::MutexLocker locker { m_mutex };
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

    Sync::MutexLocker locker { m_mutex };

    Optional<u32> reusable_slot_index;
    Optional<u32> free_slot_index;
    for (u32 index = 0; index < m_slots.size(); index++) {
        auto const& slot = m_slots[index];
        if (slot.hold_count != 0)
            continue;
        if (!free_slot_index.has_value())
            free_slot_index = index;
        if (slot.buffer.is_valid() && slot.buffer.size() - slot_data_offset() == byte_count) {
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
        drop_slot_storage_while_locked(*chosen_slot);
        chosen_slot->buffer = buffer_result.release_value();
        chosen_slot->allocated_buffer_id++;
        new (chosen_slot->buffer.data<void>()) SlotHeader;
        m_allocated_bytes += chosen_slot->buffer.size();
    }

    publish_acquisition_while_locked(*chosen_slot);

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
    return try_adopt_slot(acquired_slot.index, acquired_slot.slot_acquisition_id, acquired_slot.allocated_buffer_id);
}

void VideoFrameEntryLedger::publish_acquisition_while_locked(Slot& slot)
{
    m_shed_storage_on_release = false;
    if (slot.surface != nullptr)
        slot.surface_use = slot.surface->begin_use();
    slot.hold_count = 1;
    slot.last_slot_acquisition_id++;
    slot_header(slot.buffer).slot_acquisition_id.store(slot.last_slot_acquisition_id);
    AK::atomic_thread_fence(AK::MemoryOrder::memory_order_seq_cst);
}

Optional<u32> VideoFrameEntryLedger::try_grow_while_locked()
{
    if (m_slots.try_append({}).is_error())
        return {};
    return static_cast<u32>(m_slots.size() - 1);
}

RefPtr<VideoSurface> VideoFrameEntryLedger::slot_surface(u32 slot_index) const
{
    Sync::MutexLocker locker { m_mutex };
    VERIFY(m_slots[slot_index].hold_count > 0);
    return m_slots[slot_index].surface;
}

ErrorOr<NonnullRefPtr<PooledVideoFrameSlot>> VideoFrameEntryLedger::try_adopt_slot(u32 slot_index, u64 slot_acquisition_id, u64 allocated_buffer_id)
{
    auto slot_result = try_make_ref_counted<PooledVideoFrameSlot>(*this, slot_index, slot_acquisition_id, allocated_buffer_id);
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
    Sync::MutexLocker locker { m_mutex };
    m_shed_storage_on_release = true;
    for (auto& slot : m_slots) {
        if (slot.hold_count == 0)
            drop_slot_storage_while_locked(slot);
    }
}

void VideoFrameEntryLedger::add_hold(u32 slot_index)
{
    Sync::MutexLocker locker { m_mutex };
    VERIFY(m_slots[slot_index].hold_count > 0);
    m_slots[slot_index].hold_count++;
}

void VideoFrameEntryLedger::release_hold(u32 slot_index)
{
    {
        Sync::MutexLocker locker { m_mutex };
        auto& slot = m_slots[slot_index];
        VERIFY(slot.hold_count > 0);
        if (--slot.hold_count > 0)
            return;
        // The decoder is free to hand this surface back now, and the slot is still here to recognize it by.
        slot.surface_use = {};
        if (m_shed_storage_on_release)
            drop_slot_storage_while_locked(slot);
    }

    if (m_slot_freed_callback)
        m_slot_freed_callback();
}

Core::AnonymousBuffer VideoFrameEntryLedger::slot_buffer(u32 slot_index) const
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

ErrorOr<NonnullRefPtr<VideoFrameSurfacePool>> VideoFrameSurfacePool::create()
{
    return adopt_nonnull_ref_or_enomem(new (nothrow) VideoFrameSurfacePool());
}

Optional<VideoFrameSurfacePool::AcquiredSlot> VideoFrameSurfacePool::try_acquire(NonnullRefPtr<VideoSurface> const& surface)
{
    Sync::MutexLocker locker { m_mutex };

    auto slot_index = m_slot_indices_by_surface_id.get(surface->id()).copy();

    if (!slot_index.has_value()) {
        Optional<u32> unused_slot_index;
        Optional<u32> evictable_slot_index;
        for (u32 index = 0; index < m_slots.size(); index++) {
            auto const& slot = m_slots[index];
            if (slot.hold_count != 0)
                continue;
            if (slot.surface == nullptr) {
                unused_slot_index = index;
                break;
            }
            if (!evictable_slot_index.has_value() || slot.last_slot_acquisition_id < m_slots[*evictable_slot_index].last_slot_acquisition_id)
                evictable_slot_index = index;
        }

        if (!unused_slot_index.has_value() && m_slots.size() < MAX_SLOT_COUNT)
            unused_slot_index = try_grow_while_locked();
        // A full pool gives up the surface identity it is least likely to be handed again.
        if (!unused_slot_index.has_value())
            unused_slot_index = evictable_slot_index;
        if (!unused_slot_index.has_value())
            return {};

        auto buffer_result = Core::AnonymousBuffer::create_with_size(slot_data_offset());
        if (buffer_result.is_error())
            return {};
        if (m_slot_indices_by_surface_id.try_set(surface->id(), *unused_slot_index).is_error())
            return {};

        auto& unused_slot = m_slots[*unused_slot_index];
        drop_slot_storage_while_locked(unused_slot);
        unused_slot.buffer = buffer_result.release_value();
        new (unused_slot.buffer.data<void>()) SlotHeader;
        unused_slot.allocated_buffer_id++;
        slot_index = unused_slot_index;
    }

    auto& slot = m_slots[*slot_index];
    VERIFY(slot.hold_count == 0);
    slot.surface = surface;
    publish_acquisition_while_locked(slot);

    return AcquiredSlot {
        .index = *slot_index,
        .slot_acquisition_id = slot.last_slot_acquisition_id,
        .allocated_buffer_id = slot.allocated_buffer_id,
    };
}

void VideoFrameSurfacePool::drop_slot_storage_while_locked(Slot& slot)
{
    if (slot.surface == nullptr)
        return;
    m_slot_indices_by_surface_id.remove(slot.surface->id());
    slot.surface = nullptr;
}

ErrorOr<NonnullRefPtr<PooledVideoFrameSlot>> VideoFrameSurfacePool::try_adopt_acquired_slot(AcquiredSlot const& acquired_slot)
{
    return try_adopt_slot(acquired_slot.index, acquired_slot.slot_acquisition_id, acquired_slot.allocated_buffer_id);
}

ResolvedVideoFrameSlot::ResolvedVideoFrameSlot(Core::AnonymousBuffer slot_buffer, RefPtr<VideoSurface> surface, VideoFramePoolID pool_id, u32 slot_index, u64 slot_acquisition_id, Function<void()> on_release)
    : m_slot_buffer(move(slot_buffer))
    , m_surface(move(surface))
    , m_pool_id(pool_id)
    , m_slot_index(slot_index)
    , m_slot_acquisition_id(slot_acquisition_id)
    , m_on_release(move(on_release))
{
    // The slot this was resolved from can be released from under it, so this is what keeps the pixels it reads from
    // being decoded into again.
    if (m_surface != nullptr)
        m_surface_use = m_surface->begin_use();
}

ResolvedVideoFrameSlot::~ResolvedVideoFrameSlot()
{
    if (m_on_release)
        m_on_release();
}

bool ResolvedVideoFrameSlot::revalidate() const
{
    AK::atomic_thread_fence(AK::MemoryOrder::memory_order_acquire);
    return slot_header(m_slot_buffer).slot_acquisition_id.load() == m_slot_acquisition_id;
}

ErrorOr<Gfx::YUVData> yuv_data_in_slot_buffer(Core::AnonymousBuffer const& slot_buffer, Gfx::IntSize size, u8 bit_depth, Subsampling subsampling, CodingIndependentCodePoints const& cicp)
{
    if (!slot_buffer.is_valid() || slot_buffer.size() <= slot_data_offset())
        return Error::from_string_literal("Invalid video frame slot buffer");

    auto layout = TRY(frame_plane_layout(size, bit_depth, subsampling));
    auto capacity = slot_buffer.size() - slot_data_offset();
    if (layout.total_byte_count > capacity)
        return Error::from_string_literal("Video frame format does not fit its slot");

    auto bytes = Bytes { const_cast<u8*>(slot_buffer.data<u8 const>()) + slot_data_offset(), capacity };
    return Gfx::YUVData::create(size, bit_depth, subsampling, cicp, bytes.slice(0, layout.y_size), bytes.slice(layout.u_offset, layout.u_size), bytes.slice(layout.v_offset, layout.v_size));
}

ErrorOr<NonnullRefPtr<VideoFrame>> resolve_frame_from_slot(Core::AnonymousBuffer const& slot_buffer, RefPtr<VideoSurface> surface, VideoFrameHandle const& handle, Function<void()> on_release)
{
    if (!slot_buffer.is_valid() || slot_buffer.size() < slot_data_offset())
        return Error::from_string_literal("Invalid video frame slot buffer");

    // A surface holds the pixels itself, so only an inline framebuffer has a plane layout to validate.
    if (surface == nullptr)
        TRY(yuv_data_in_slot_buffer(slot_buffer, handle.size, handle.bit_depth, handle.subsampling, handle.cicp));

    if (slot_header(slot_buffer).slot_acquisition_id.load(AK::MemoryOrder::memory_order_acquire) != handle.slot_acquisition_id)
        return Error::from_string_literal("VideoFrameHandle refers to a recycled slot");

    auto resolved_slot = TRY(try_make_ref_counted<ResolvedVideoFrameSlot>(slot_buffer, move(surface), handle.pool_id, handle.slot_index, handle.slot_acquisition_id, move(on_release)));
    return try_make_ref_counted<VideoFrame>(handle.timestamp, handle.duration, handle.size.to_type<u32>(), handle.bit_depth, handle.subsampling, handle.cicp, move(resolved_slot));
}

NonnullRefPtr<VideoFrameSlotDirectory> VideoFrameSlotDirectory::create()
{
    return adopt_ref(*new VideoFrameSlotDirectory());
}

VideoFrameSlotDirectory::~VideoFrameSlotDirectory() = default;

void VideoFrameSlotDirectory::set_on_slots_changed(Function<void()> on_slots_changed)
{
    m_on_slots_changed = move(on_slots_changed);
}

void VideoFrameSlotDirectory::notify_slot_announced(VideoFramePoolID pool_id, u32 slot_index, Core::AnonymousBuffer slot_buffer, RefPtr<VideoSurface> surface)
{
    if (!slot_buffer.is_valid() || slot_buffer.size() < slot_data_offset())
        return;
    m_slots_by_pool_id.ensure(pool_id).set(slot_index, AnnouncedSlot { move(slot_buffer), move(surface) });
    if (m_on_slots_changed)
        m_on_slots_changed();
}

void VideoFrameSlotDirectory::notify_pool_retired(VideoFramePoolID pool_id)
{
    m_slots_by_pool_id.remove(pool_id);
}

void VideoFrameSlotDirectory::notify_all_pools_retired()
{
    m_slots_by_pool_id.clear();
}

RefPtr<VideoFrame> VideoFrameSlotDirectory::resolve_frame(VideoFrameHandle const& handle, Function<void()> on_release) const
{
    auto slots = m_slots_by_pool_id.get(handle.pool_id);
    if (!slots.has_value())
        return nullptr;
    auto slot = slots->get(handle.slot_index);
    if (!slot.has_value())
        return nullptr;

    // A frame's handle reaches us through the frame ring while the buffer it was written into is announced by
    // message, so a handle can arrive before the buffer it belongs to. Acquisition IDs count up per slot across
    // every buffer it is given, so one running ahead of the announced buffer's is that wait rather than a loss.
    if (slot_header(slot->buffer).slot_acquisition_id.load(AK::MemoryOrder::memory_order_acquire) < handle.slot_acquisition_id)
        return nullptr;

    auto frame_or_error = resolve_frame_from_slot(slot->buffer, slot->surface, handle, move(on_release));
    if (frame_or_error.is_error()) {
        dbgln("VideoFrameSlotDirectory: Slot {} of pool {} was announced but did not resolve: {}", handle.slot_index, handle.pool_id, frame_or_error.error());
        return nullptr;
    }
    return frame_or_error.release_value();
}

}
