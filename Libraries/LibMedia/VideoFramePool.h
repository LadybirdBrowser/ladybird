/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibMedia/Export.h>
#include <LibSync/Mutex.h>

namespace Media {

class PooledVideoFrameSlot;

// A pool of shared-memory framebuffers that are allocated on demand up to a limited budget. Each slot's buffer fd must
// be transmitted over IPC before it can be used, but once that has happened, it will be reused until a different size
// is needed.
class MEDIA_API VideoFramePool : public AtomicRefCounted<VideoFramePool> {
public:
    static constexpr u32 MIN_SLOT_COUNT = 4;
    static constexpr u32 MAX_SLOT_COUNT = 8;
    static constexpr size_t DEFAULT_BYTE_BUDGET = 256 * MiB;

    // The slot-freed callback is invoked without the pool's lock held whenever a slot becomes free, possibly from
    // several threads at once.
    static ErrorOr<NonnullRefPtr<VideoFramePool>> create(Function<void()> slot_freed_callback = nullptr, size_t byte_budget = DEFAULT_BYTE_BUDGET);

    struct AcquiredSlot {
        u32 index { 0 };
        u64 slot_acquisition_id { 0 };
        Bytes bytes;
    };
    // Gets a free slot with the requested capacity if it is available, reallocating the buffer if needed.
    Optional<AcquiredSlot> try_acquire(size_t byte_count);
    ErrorOr<NonnullRefPtr<PooledVideoFrameSlot>> try_adopt_acquired_slot(AcquiredSlot const&);

    void add_hold(u32 slot_index);
    void release_hold(u32 slot_index);

    // Marks the buffers for freeing when their hold count drops to zero. This marker is cleared upon the next acquire.
    void shed_buffers();

    size_t allocated_byte_count() const;

private:
    VideoFramePool(Function<void()> slot_freed_callback, size_t byte_budget);

    struct Slot {
        Core::AnonymousBuffer buffer;
        u64 last_slot_acquisition_id { 0 };
        u32 hold_count { 0 };
    };

    u32 held_slot_count_while_locked() const;
    void drop_slot_buffer_while_locked(Slot&);
    void free_excess_buffers_while_locked();

    Function<void()> const m_slot_freed_callback;
    size_t const m_byte_budget { 0 };

    mutable Sync::Mutex m_mutex;
    Array<Slot, MAX_SLOT_COUNT> m_slots;
    size_t m_allocated_bytes { 0 };
    bool m_shed_buffers_on_release { false };
};

// A strong reference to a slot in a frame pool to be used within a single process.
class PooledVideoFrameSlot : public AtomicRefCounted<PooledVideoFrameSlot> {
public:
    PooledVideoFrameSlot(NonnullRefPtr<VideoFramePool> pool, u32 slot_index, u64 slot_acquisition_id)
        : m_pool(move(pool))
        , m_slot_index(slot_index)
        , m_slot_acquisition_id(slot_acquisition_id)
    {
    }

    ~PooledVideoFrameSlot()
    {
        m_pool->release_hold(m_slot_index);
    }

    VideoFramePool& pool() { return m_pool; }
    u32 slot_index() const { return m_slot_index; }
    u64 slot_acquisition_id() const { return m_slot_acquisition_id; }

private:
    NonnullRefPtr<VideoFramePool> m_pool;
    u32 m_slot_index { 0 };
    u64 m_slot_acquisition_id { 0 };
};


}
