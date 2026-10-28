/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <AK/Mutex.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/YUVData.h>
#include <LibMediaCodecs/Export.h>
#include <LibMediaCodecs/VideoFrameMetadata.h>
#include <LibMediaCodecs/VideoSurface.h>

namespace Media {

class PooledVideoFrameSlot;

// The lifetime bookkeeping shared by every kind of frame pool: each slot stays held while any frame stored in it is.
class MEDIACODECS_API VideoFrameEntryLedger : public AtomicRefCounted<VideoFrameEntryLedger> {
public:
    virtual ~VideoFrameEntryLedger();

    // The callback runs on whichever thread frees a slot, without the ledger's lock held, and may run concurrently
    // with itself when separate slots are freed at once.
    void set_slot_freed_callback(Function<void()>);

    void add_hold(u32 slot_index);
    void release_hold(u32 slot_index);

    // Marks each slot's storage for freeing when its hold count drops to zero. This marker is cleared upon the
    // next acquire.
    void shed_storage();

    // The buffer backing a held slot, for lending to consumer processes.
    Core::AnonymousBuffer slot_buffer(u32 slot_index) const;

    // The surface backing a held slot, null when the slot's pixels live in its buffer instead.
    RefPtr<VideoSurface> slot_surface(u32 slot_index) const;

protected:
    VideoFrameEntryLedger() = default;

    ErrorOr<NonnullRefPtr<PooledVideoFrameSlot>> try_adopt_slot(u32 slot_index);

    struct Slot {
        Core::AnonymousBuffer buffer;
        // Only a held slot has a surface, and the contents the decoder gave with it, which go with the last hold.
        RefPtr<VideoSurface> surface;
        RefPtr<VideoSurfaceContents> contents;
        u32 hold_count { 0 };
    };

    // Appends a slot with no payload, for a pool that has no free slot to reuse.
    Optional<u32> try_grow_while_locked();

    virtual void drop_slot_storage_while_locked(Slot&) { }

    u32 held_slot_count_while_locked() const;
    void begin_hold_while_locked(Slot&);

    mutable Mutex m_mutex;
    Vector<Slot> m_slots;

private:
    Function<void()> m_slot_freed_callback;
    bool m_shed_storage_on_release { false };
};

// A pool of shared-memory framebuffers that are allocated on demand up to a limited budget. A slot's buffer is reused
// until a different size is needed.
class MEDIACODECS_API VideoFramePool final : public VideoFrameEntryLedger {
public:
    static constexpr u32 MIN_SLOT_COUNT = 4;
    static constexpr u32 MAX_SLOT_COUNT = 8;
    static constexpr size_t DEFAULT_BYTE_BUDGET = 256 * MiB;

    static ErrorOr<NonnullRefPtr<VideoFramePool>> create(size_t byte_budget = DEFAULT_BYTE_BUDGET);

    struct AcquiredSlot {
        u32 index { 0 };
        Bytes bytes;
    };
    // Gets a free slot with the requested capacity if it is available, reallocating the buffer if needed.
    Optional<AcquiredSlot> try_acquire(size_t byte_count);
    ErrorOr<NonnullRefPtr<PooledVideoFrameSlot>> try_adopt_acquired_slot(AcquiredSlot const&);

    size_t allocated_byte_count() const;

private:
    explicit VideoFramePool(size_t byte_budget);

    void drop_slot_storage_while_locked(Slot&) override;

    void free_excess_buffers_while_locked();

    size_t const m_byte_budget { 0 };
    size_t m_allocated_bytes { 0 };
};

// A pool of surfaces produced by a hardware decoder. The decoder allocates and recycles them, so this pool only holds
// each surface for as long as a frame decoded into it is held. The held slot count is what bounds how many frames a
// hardware decoder may have in flight, since the platform's own allocator will keep handing out surfaces indefinitely.
class MEDIACODECS_API VideoFrameSurfacePool final : public VideoFrameEntryLedger {
public:
    static constexpr u32 MAX_HELD_SLOT_COUNT = 8;

    static ErrorOr<NonnullRefPtr<VideoFrameSurfacePool>> create();

    struct AcquiredSlot {
        u32 index { 0 };
    };
    // Gets a free slot for the surface, holding it and the contents the decoder gave with it.
    Optional<AcquiredSlot> try_acquire(NonnullRefPtr<VideoSurface> const&, NonnullRefPtr<VideoSurfaceContents>);
    ErrorOr<NonnullRefPtr<PooledVideoFrameSlot>> try_adopt_acquired_slot(AcquiredSlot const&);

private:
    VideoFrameSurfacePool() = default;
};

// A strong reference to a slot in a frame pool to be used within a single process.
class PooledVideoFrameSlot : public AtomicRefCounted<PooledVideoFrameSlot> {
public:
    PooledVideoFrameSlot(NonnullRefPtr<VideoFrameEntryLedger> ledger, u32 slot_index)
        : m_ledger(move(ledger))
        , m_slot_index(slot_index)
    {
    }

    ~PooledVideoFrameSlot()
    {
        m_ledger->release_hold(m_slot_index);
    }

    VideoFrameEntryLedger& ledger() const { return m_ledger; }
    u32 slot_index() const { return m_slot_index; }

private:
    NonnullRefPtr<VideoFrameEntryLedger> m_ledger;
    u32 m_slot_index { 0 };
};

// The storage a frame received from another process arrived with, held until the frame is released. Exactly one of the
// pixel buffer and the surface holds the frame's pixels.
class MEDIACODECS_API ReceivedVideoFrameStorage : public AtomicRefCounted<ReceivedVideoFrameStorage> {
public:
    ReceivedVideoFrameStorage(Core::AnonymousBuffer pixel_buffer, RefPtr<VideoSurface> surface, u64 sender_id, Function<void()> on_release);
    ~ReceivedVideoFrameStorage();

    Core::AnonymousBuffer const& pixel_buffer() const { return m_pixel_buffer; }
    RefPtr<VideoSurface> const& surface() const { return m_surface; }
    // Names the peer the frame came from, so that anything kept by an ID the peer chose is never shared with another.
    u64 sender_id() const { return m_sender_id; }

private:
    Core::AnonymousBuffer m_pixel_buffer;
    RefPtr<VideoSurface> m_surface;
    u64 m_sender_id { 0 };
    Function<void()> m_on_release;
};

// Views the planes laid out within a frame's buffer, failing if they do not fit its capacity.
MEDIACODECS_API ErrorOr<Gfx::YUVData> yuv_data_in_slot_buffer(Core::AnonymousBuffer const&, Gfx::IntSize, u8 bit_depth, Subsampling, CodingIndependentCodePoints const&);

// Builds a frame from the storage another process sent along with its metadata, checking that the storage can hold
// such a frame. The release callback runs once the frame is dropped, or right away if it cannot be built.
MEDIACODECS_API ErrorOr<NonnullRefPtr<VideoFrame>> create_received_video_frame(VideoFrameMetadata const&, Core::AnonymousBuffer pixel_buffer, RefPtr<VideoSurface>, u64 sender_id, Function<void()> on_release);

}
