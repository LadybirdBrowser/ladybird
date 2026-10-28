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
#include <LibMediaCodecs/VideoFrame.h>
#include <LibMediaCodecs/VideoFrameMetadata.h>
#include <LibMediaCodecs/VideoSurface.h>

namespace Media {

// Holds a frame's pixels until the last frame sharing them is dropped, then runs the release callback once, on
// whichever thread dropped it.
class MEDIACODECS_API VideoFrameStorage final : public AtomicRefCounted<VideoFrameStorage> {
public:
    VideoFrameStorage(VideoFramePixels, u64 sender_id, Function<void()> on_release);
    ~VideoFrameStorage();

    VideoFramePixels const& pixels() const { return m_pixels; }
    // Tells the frames one peer sent apart from every other peer's in this process, and is 0 for frames decoded here.
    u64 sender_id() const { return m_sender_id; }
    // Invalid when a surface holds the pixels instead.
    Core::AnonymousBuffer pixel_buffer() const;
    // Null when a buffer holds the pixels instead.
    RefPtr<VideoSurface> surface() const;

private:
    VideoFramePixels m_pixels;
    u64 m_sender_id { 0 };
    Function<void()> m_on_release;
};

// A pool of shared-memory framebuffers, allocated on demand within a byte budget that the first MIN_SLOT_COUNT
// held slots may exceed.
class MEDIACODECS_API VideoFramePool final : public AtomicRefCounted<VideoFramePool> {
public:
    static constexpr u32 MIN_SLOT_COUNT = 4;
    static constexpr u32 MAX_SLOT_COUNT = 8;
    static constexpr size_t DEFAULT_BYTE_BUDGET = 256 * MiB;

    static ErrorOr<NonnullRefPtr<VideoFramePool>> create(size_t byte_budget = DEFAULT_BYTE_BUDGET);
    ~VideoFramePool();

    // The callback runs on whichever thread frees a slot, without the pool's lock held, and may run concurrently
    // with itself when separate slots are freed at once.
    void set_slot_freed_callback(Function<void()>);

    struct AcquiredSlot {
        NonnullRefPtr<VideoFrameStorage> storage;
        Bytes bytes;
    };
    // Gets a free slot with the requested capacity if it is available, reallocating the buffer if needed.
    Optional<AcquiredSlot> try_acquire(size_t byte_count);

    // Marks each slot's storage for freeing when its hold count drops to zero. This marker is cleared upon the
    // next acquire.
    void shed_storage();

    size_t allocated_byte_count() const;

private:
    struct Slot {
        Core::AnonymousBuffer buffer;
        bool is_held { false };
    };

    explicit VideoFramePool(size_t byte_budget);

    void release_slot(u32 slot_index);
    u32 held_slot_count_while_locked() const;
    void drop_slot_storage_while_locked(Slot&);
    void free_excess_buffers_while_locked();

    mutable Mutex m_mutex;
    Vector<Slot> m_slots;
    Function<void()> m_slot_freed_callback;
    bool m_shed_storage_on_release { false };

    size_t const m_byte_budget { 0 };
    size_t m_allocated_bytes { 0 };
};

class MEDIACODECS_API VideoSurfaceBudget final : public AtomicRefCounted<VideoSurfaceBudget> {
public:
    static constexpr u32 MAX_HELD_SURFACE_COUNT = 8;

    ~VideoSurfaceBudget();

    // The callback runs on whichever thread releases a surface, without the budget's lock held, and may run
    // concurrently with itself.
    void set_storage_freed_callback(Function<void()>);

    // Null once MAX_HELD_SURFACE_COUNT surfaces are held, until one is released.
    RefPtr<VideoFrameStorage> try_hold(NonnullRefPtr<VideoSurface>, NonnullRefPtr<VideoSurfaceContents>);

private:
    void release_hold();

    Mutex m_mutex;
    u32 m_held_surface_count { 0 };
    Function<void()> m_storage_freed_callback;
};

// Views the planes laid out within a frame's buffer, failing if they do not fit its capacity.
MEDIACODECS_API ErrorOr<Gfx::YUVData> yuv_data_in_buffer(Core::AnonymousBuffer const&, Gfx::IntSize, u8 bit_depth, Subsampling, CodingIndependentCodePoints const&);

// Refuses a frame the pixels could not hold, releasing them.
MEDIACODECS_API ErrorOr<NonnullRefPtr<VideoFrame>> create_received_video_frame(VideoFrameMetadata const&, VideoFramePixels, u64 sender_id, Function<void()> on_release);

}
