/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Span.h>
#include <AK/Types.h>

namespace Web::WebAudio::Render {

// StreamTransport is a low-level building block for WebAudio data-plane transport.
// It is designed for shared memory usage across processes with no locks and no render-thread blocking.
//
// This header intentionally focuses on:
// - stable shared-memory layouts
// - overflow policies
// - small atomic helper functions
//
// Higher-level ownership, IPC, and lifecycle live elsewhere.

enum class StreamKind : u8 {
    Ring,
    Packet,
};

enum class StreamOverflowPolicy : u8 {
    // When full, overwrite oldest unread frames.
    DropOldest,

    // When full, reject new frames.
    DropNewest,

    // Never lose frames. Producers may wait, but only on non-realtime threads.
    Lossless,
};

// RingStream is an SPSC ring of interleaved f32 frames.
// All counters are monotonic frame indices.
static constexpr u32 ring_stream_version = 2;

struct RingStreamHeader {
    u32 version;

    // Fixed format for the lifetime of the stream.
    u32 sample_rate_hz;
    u32 channel_count;

    // Storage channel count. Must be >= channel_count.
    u32 channel_capacity;

    // Total frame capacity of the ring.
    u64 capacity_frames;

    // Consumer-written, producer-read.
    u64 read_frame;

    // Producer-written, consumer-read.
    u64 write_frame;

    // Consumer-maintained statistics.
    u64 overrun_frames_total;

    // Optional timeline metadata. A nonzero sample rate indicates timing is valid.
    u64 timeline_generation;
    u32 timeline_sample_rate;
    u32 reserved0;
    u64 timeline_media_start_frame;
    u64 timeline_media_start_at_ring_frame;

    // Flags. Stored in reserved1 to keep the header size stable.
    // Bit 0: end-of-stream (producer will not write any more frames).
    u64 reserved1;

    // Producer timestamp anchors.
    // These are best-effort hints to allow consumers to correlate ring frames with the
    // media timeline (e.g. for A/V sync). They are not used for discontinuity detection.
    // - producer_timestamp_generation: timeline_generation value associated with the anchor.
    // - producer_media_start_frame: media-frame index of the first frame in the pushed block.
    // - producer_media_start_at_ring_frame: ring-frame index corresponding to producer_media_start_frame.
    u64 producer_timestamp_generation;
    u64 producer_media_start_frame;
    u64 producer_media_start_at_ring_frame;
};

static_assert(sizeof(RingStreamHeader) % alignof(f32) == 0);

// RingStreamView is a lightweight, non-owning view of the shared-memory ring.
// Ownership/lifetime of the mapped memory is managed by the session/backend.
struct RingStreamView {
    RingStreamHeader* header { nullptr };
    Span<f32> interleaved_frames;
};

// Layout helpers
inline size_t ring_stream_bytes_for_data(u32 channel_capacity, u64 capacity_frames)
{
    return static_cast<size_t>(channel_capacity) * static_cast<size_t>(capacity_frames) * sizeof(f32);
}

inline size_t ring_stream_bytes_total(u32 channel_capacity, u64 capacity_frames)
{
    return sizeof(RingStreamHeader) + ring_stream_bytes_for_data(channel_capacity, capacity_frames);
}

// Atomic helpers. These are used instead of C++ atomics to keep the layout trivially shared.
inline u64 ring_stream_load_read_frame(RingStreamHeader& header)
{
    return AK::atomic_load(&header.read_frame, AK::MemoryOrder::memory_order_acquire);
}

inline u64 ring_stream_load_write_frame(RingStreamHeader& header)
{
    return AK::atomic_load(&header.write_frame, AK::MemoryOrder::memory_order_acquire);
}

inline void ring_stream_store_read_frame(RingStreamHeader& header, u64 value)
{
    AK::atomic_store(&header.read_frame, value, AK::MemoryOrder::memory_order_release);
}

inline void ring_stream_store_write_frame(RingStreamHeader& header, u64 value)
{
    AK::atomic_store(&header.write_frame, value, AK::MemoryOrder::memory_order_release);
}

inline size_t ring_stream_available_frames(RingStreamHeader const& header, u64 read_frame, u64 write_frame)
{
    if (write_frame <= read_frame)
        return 0;
    u64 available = write_frame - read_frame;
    available = available < header.capacity_frames ? available : header.capacity_frames;
    return static_cast<size_t>(available);
}

inline bool ring_stream_consumer_detect_and_fix_overrun(RingStreamHeader& header, u64& in_out_read_frame, u64 write_frame)
{
    if (write_frame <= in_out_read_frame)
        return false;

    u64 unread = write_frame - in_out_read_frame;
    if (unread <= header.capacity_frames)
        return false;

    u64 new_read = write_frame - header.capacity_frames;
    u64 dropped = new_read - in_out_read_frame;

    header.overrun_frames_total += dropped;
    in_out_read_frame = new_read;
    ring_stream_store_read_frame(header, new_read);
    return true;
}

static constexpr u64 ring_stream_flag_end_of_stream = 1ull << 0;

inline u64 ring_stream_load_flags(RingStreamHeader& header)
{
    return AK::atomic_load(&header.reserved1, AK::MemoryOrder::memory_order_relaxed);
}

inline void ring_stream_store_flags(RingStreamHeader& header, u64 flags)
{
    AK::atomic_store(&header.reserved1, flags, AK::MemoryOrder::memory_order_relaxed);
}

inline void ring_stream_set_flag(RingStreamHeader& header, u64 flag)
{
    u64 flags = ring_stream_load_flags(header);
    ring_stream_store_flags(header, flags | flag);
}

inline void ring_stream_clear_flag(RingStreamHeader& header, u64 flag)
{
    u64 flags = ring_stream_load_flags(header);
    ring_stream_store_flags(header, flags & ~flag);
}

struct RingStreamProducerTimestampAnchor {
    u64 generation { 0 };
    u64 media_start_frame { 0 };
    u64 media_start_at_ring_frame { 0 };
};

inline void ring_stream_store_producer_timestamp_anchor(RingStreamHeader& header, RingStreamProducerTimestampAnchor const& anchor)
{
    AK::atomic_store(&header.producer_media_start_frame, anchor.media_start_frame, AK::MemoryOrder::memory_order_relaxed);
    AK::atomic_store(&header.producer_media_start_at_ring_frame, anchor.media_start_at_ring_frame, AK::MemoryOrder::memory_order_relaxed);
    AK::atomic_store(&header.producer_timestamp_generation, anchor.generation, AK::MemoryOrder::memory_order_relaxed);
}

inline RingStreamProducerTimestampAnchor ring_stream_load_producer_timestamp_anchor(RingStreamHeader& header)
{
    RingStreamProducerTimestampAnchor anchor;
    anchor.media_start_frame = AK::atomic_load(&header.producer_media_start_frame, AK::MemoryOrder::memory_order_relaxed);
    anchor.media_start_at_ring_frame = AK::atomic_load(&header.producer_media_start_at_ring_frame, AK::MemoryOrder::memory_order_relaxed);
    anchor.generation = AK::atomic_load(&header.producer_timestamp_generation, AK::MemoryOrder::memory_order_relaxed);
    return anchor;
}

inline void ring_stream_clear_producer_timestamp_anchor(RingStreamHeader& header)
{
    ring_stream_store_producer_timestamp_anchor(header, {});
}

}
