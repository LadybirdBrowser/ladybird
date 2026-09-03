/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/YUVData.h>
#include <LibMedia/Export.h>
#include <LibMedia/VideoFrameHandle.h>
#include <LibSync/Mutex.h>

namespace Media {

class PooledVideoFrameSlot;

// The identity and lifetime bookkeeping shared by every kind of frame pool. Each slot owns a shared-memory buffer
// beginning with the acquisition ID that a remote consumer validates its handle against; what follows the ID is the
// pool's business.
class MEDIA_API VideoFrameEntryLedger : public AtomicRefCounted<VideoFrameEntryLedger> {
public:
    virtual ~VideoFrameEntryLedger();

    VideoFramePoolID id() const { return m_id; }

    void add_hold(u32 slot_index);
    void release_hold(u32 slot_index);

    // The buffer backing a held slot, for lending to consumer processes.
    Core::AnonymousBuffer slot_buffer(u32 slot_index) const;

protected:
    // The slot-freed callback runs on whichever thread frees a slot, without the ledger's lock held, and may run
    // concurrently with itself when separate slots are freed at once.
    explicit VideoFrameEntryLedger(Function<void()> slot_freed_callback);

    struct Slot {
        Core::AnonymousBuffer buffer;
        u64 last_slot_acquisition_id { 0 };
        u64 allocated_buffer_id { 0 };
        u32 hold_count { 0 };
    };

    virtual void slot_freed_while_locked(Slot&) { }

    u32 held_slot_count_while_locked() const;
    void publish_acquisition_while_locked(Slot&);

    mutable Sync::Mutex m_mutex;
    Vector<Slot> m_slots;

private:
    VideoFramePoolID const m_id;
    Function<void()> const m_slot_freed_callback;
};

// A pool of shared-memory framebuffers that are allocated on demand up to a limited budget. Each slot's buffer fd must
// be transmitted over IPC before it can be used, but once that has happened, it will be reused until a different size
// is needed.
class MEDIA_API VideoFramePool final : public VideoFrameEntryLedger {
public:
    static constexpr u32 MIN_SLOT_COUNT = 4;
    static constexpr u32 MAX_SLOT_COUNT = 8;
    static constexpr size_t DEFAULT_BYTE_BUDGET = 256 * MiB;

    static ErrorOr<NonnullRefPtr<VideoFramePool>> create(Function<void()> slot_freed_callback = nullptr, size_t byte_budget = DEFAULT_BYTE_BUDGET);

    struct AcquiredSlot {
        u32 index { 0 };
        u64 slot_acquisition_id { 0 };
        u64 allocated_buffer_id { 0 };
        Bytes bytes;
    };
    // Gets a free slot with the requested capacity if it is available, reallocating the buffer if needed.
    Optional<AcquiredSlot> try_acquire(size_t byte_count);
    ErrorOr<NonnullRefPtr<PooledVideoFrameSlot>> try_adopt_acquired_slot(AcquiredSlot const&);

    // Marks the buffers for freeing when their hold count drops to zero. This marker is cleared upon the next acquire.
    void shed_buffers();

    size_t allocated_byte_count() const;

private:
    VideoFramePool(Function<void()> slot_freed_callback, size_t byte_budget);

    void slot_freed_while_locked(Slot&) override;

    void drop_slot_buffer_while_locked(Slot&);
    void free_excess_buffers_while_locked();

    size_t const m_byte_budget { 0 };
    size_t m_allocated_bytes { 0 };
    bool m_shed_buffers_on_release { false };
};

// A strong reference to a slot in a frame pool to be used within a single process.
class PooledVideoFrameSlot : public AtomicRefCounted<PooledVideoFrameSlot> {
public:
    PooledVideoFrameSlot(NonnullRefPtr<VideoFrameEntryLedger> ledger, u32 slot_index, u64 slot_acquisition_id, u64 allocated_buffer_id)
        : m_ledger(move(ledger))
        , m_slot_index(slot_index)
        , m_slot_acquisition_id(slot_acquisition_id)
        , m_allocated_buffer_id(allocated_buffer_id)
    {
    }

    ~PooledVideoFrameSlot()
    {
        m_ledger->release_hold(m_slot_index);
    }

    VideoFrameEntryLedger& ledger() const { return m_ledger; }
    u32 slot_index() const { return m_slot_index; }
    u64 slot_acquisition_id() const { return m_slot_acquisition_id; }
    u64 allocated_buffer_id() const { return m_allocated_buffer_id; }

private:
    NonnullRefPtr<VideoFrameEntryLedger> m_ledger;
    u32 m_slot_index { 0 };
    u64 m_slot_acquisition_id { 0 };
    u64 m_allocated_buffer_id { 0 };
};

// A strong reference to a slot in a frame pool resolved on the remote side.
class ResolvedVideoFrameSlot : public AtomicRefCounted<ResolvedVideoFrameSlot> {
public:
    ResolvedVideoFrameSlot(Core::AnonymousBuffer slot_buffer, VideoFramePoolID pool_id, u32 slot_index, u64 slot_acquisition_id, Function<void()> on_release)
        : m_slot_buffer(move(slot_buffer))
        , m_pool_id(pool_id)
        , m_slot_index(slot_index)
        , m_slot_acquisition_id(slot_acquisition_id)
        , m_on_release(move(on_release))
    {
    }

    ~ResolvedVideoFrameSlot()
    {
        if (m_on_release)
            m_on_release();
    }

    bool revalidate() const;

    Core::AnonymousBuffer slot_buffer() const { return m_slot_buffer; }
    VideoFramePoolID pool_id() const { return m_pool_id; }
    u32 slot_index() const { return m_slot_index; }
    u64 slot_acquisition_id() const { return m_slot_acquisition_id; }

private:
    Core::AnonymousBuffer m_slot_buffer;
    VideoFramePoolID m_pool_id { 0 };
    u32 m_slot_index { 0 };
    u64 m_slot_acquisition_id { 0 };
    Function<void()> m_on_release;
};

// Views the planes laid out within a slot's buffer, failing if they do not fit its capacity.
MEDIA_API ErrorOr<Gfx::YUVData> yuv_data_in_slot_buffer(Core::AnonymousBuffer const&, Gfx::IntSize, u8 bit_depth, Subsampling, CodingIndependentCodePoints const&);

// Resolves a frame handle against the slot framebuffer it refers to, validating the slot's acquisition ID against the
// handle's. The resolved frame revalidates it again after its pixels are consumed, seqlock-style, discarding reads of
// recycled slots.
MEDIA_API ErrorOr<NonnullRefPtr<VideoFrame>> resolve_frame_from_slot_buffer(Core::AnonymousBuffer const& slot_buffer, VideoFrameHandle const&, Function<void()> on_release);

// Tracks all the active slot framebuffers from a VideoFramePool via announcement and retirement notifications. Used to
// resolve frames on a remote process. Resolved frames' lifetimes are guaranteed until they are released.
class MEDIA_API VideoFrameSlotDirectory : public AtomicRefCounted<VideoFrameSlotDirectory> {
public:
    static NonnullRefPtr<VideoFrameSlotDirectory> create();

    void set_on_slots_changed(Function<void()>);

    void notify_slot_announced(VideoFramePoolID, u32 slot_index, Core::AnonymousBuffer slot_buffer);
    void notify_pool_retired(VideoFramePoolID);

    RefPtr<VideoFrame> resolve_frame(VideoFrameHandle const&, Function<void()> on_release) const;

private:
    VideoFrameSlotDirectory() = default;

    HashMap<VideoFramePoolID, HashMap<u32, Core::AnonymousBuffer>> m_slot_buffers_by_pool_id;
    Function<void()> m_on_slots_changed;
};

}
