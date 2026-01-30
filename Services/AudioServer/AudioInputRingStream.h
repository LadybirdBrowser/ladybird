/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Span.h>
#include <AK/Types.h>

namespace AudioServer {

enum class StreamOverflowPolicy : u8 {
    DropOldest,
    DropNewest,
    Lossless,
};

static constexpr u32 ring_stream_version = 2;

struct RingStreamHeader {
    u32 version;

    u32 sample_rate_hz;
    u32 channel_count;
    u32 channel_capacity;
    u64 capacity_frames;

    u64 read_frame;
    u64 write_frame;
    u64 overrun_frames_total;

    u64 timeline_generation;
    u32 timeline_sample_rate;
    u32 reserved0;
    u64 timeline_media_start_frame;
    u64 timeline_media_start_at_ring_frame;

    u64 reserved1;

    u64 producer_timestamp_generation;
    u64 producer_media_start_frame;
    u64 producer_media_start_at_ring_frame;
};

static_assert(sizeof(RingStreamHeader) % alignof(float) == 0);

struct RingStreamView {
    RingStreamHeader* header { nullptr };
    Span<float> interleaved_frames;
};

inline size_t ring_stream_bytes_for_data(u32 channel_capacity, u64 capacity_frames)
{
    return static_cast<size_t>(channel_capacity) * static_cast<size_t>(capacity_frames) * sizeof(float);
}

inline size_t ring_stream_bytes_total(u32 channel_capacity, u64 capacity_frames)
{
    return sizeof(RingStreamHeader) + ring_stream_bytes_for_data(channel_capacity, capacity_frames);
}

inline void ring_stream_initialize_header(RingStreamHeader& header, u32 sample_rate_hz, u32 channel_count, u32 channel_capacity, u64 capacity_frames)
{
    header.version = ring_stream_version;
    header.sample_rate_hz = sample_rate_hz;
    header.channel_count = channel_count;
    header.channel_capacity = channel_capacity;
    header.capacity_frames = capacity_frames;

    AK::atomic_store(&header.read_frame, static_cast<u64>(0), AK::MemoryOrder::memory_order_release);
    AK::atomic_store(&header.write_frame, static_cast<u64>(0), AK::MemoryOrder::memory_order_release);

    header.overrun_frames_total = 0;
    AK::atomic_store(&header.timeline_generation, static_cast<u64>(1), AK::MemoryOrder::memory_order_release);
    AK::atomic_store(&header.timeline_sample_rate, static_cast<u32>(0), AK::MemoryOrder::memory_order_relaxed);
    AK::atomic_store(&header.timeline_media_start_frame, static_cast<u64>(0), AK::MemoryOrder::memory_order_relaxed);
    AK::atomic_store(&header.timeline_media_start_at_ring_frame, static_cast<u64>(0), AK::MemoryOrder::memory_order_relaxed);

    AK::atomic_store(&header.reserved1, static_cast<u64>(0), AK::MemoryOrder::memory_order_relaxed);
    AK::atomic_store(&header.producer_timestamp_generation, static_cast<u64>(0), AK::MemoryOrder::memory_order_relaxed);
    AK::atomic_store(&header.producer_media_start_frame, static_cast<u64>(0), AK::MemoryOrder::memory_order_relaxed);
    AK::atomic_store(&header.producer_media_start_at_ring_frame, static_cast<u64>(0), AK::MemoryOrder::memory_order_relaxed);
}

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

inline size_t ring_stream_try_push_interleaved(RingStreamView view, ReadonlySpan<float> interleaved_samples, u32 input_channel_count, StreamOverflowPolicy overflow_policy)
{
    if (!view.header)
        return 0;
    if (input_channel_count == 0)
        return 0;

    size_t input_frame_count = interleaved_samples.size() / input_channel_count;
    if (input_frame_count == 0)
        return 0;

    RingStreamHeader& header = *view.header;
    u64 capacity_frames = header.capacity_frames;
    u32 channel_capacity = header.channel_capacity;
    if (capacity_frames == 0 || channel_capacity == 0)
        return 0;

    u64 read_frame = ring_stream_load_read_frame(header);
    u64 write_frame = ring_stream_load_write_frame(header);
    u64 used = write_frame > read_frame ? (write_frame - read_frame) : 0;
    if (used > capacity_frames) {
        read_frame = write_frame - capacity_frames;
        ring_stream_store_read_frame(header, read_frame);
        used = capacity_frames;
    }

    size_t frames_to_write = input_frame_count;
    size_t skipped_frames = 0;
    if (frames_to_write > capacity_frames) {
        if (overflow_policy == StreamOverflowPolicy::DropNewest)
            return 0;
        skipped_frames = frames_to_write - static_cast<size_t>(capacity_frames);
        frames_to_write = static_cast<size_t>(capacity_frames);
    }

    size_t available = used < capacity_frames ? static_cast<size_t>(capacity_frames - used) : 0;
    if (frames_to_write > available) {
        if (overflow_policy == StreamOverflowPolicy::DropNewest)
            return 0;
        if (overflow_policy == StreamOverflowPolicy::Lossless)
            return 0;

        size_t drop = frames_to_write - available;
        read_frame += drop;
        ring_stream_store_read_frame(header, read_frame);
    }

    size_t channel_capacity_size = static_cast<size_t>(channel_capacity);
    size_t start_frame_index = static_cast<size_t>(write_frame % capacity_frames);
    size_t first_chunk_frames = min(frames_to_write, static_cast<size_t>(capacity_frames - start_frame_index));
    size_t second_chunk_frames = frames_to_write - first_chunk_frames;

    ReadonlySpan<float> input = interleaved_samples.slice(skipped_frames * input_channel_count);
    size_t input_offset = 0;

    size_t first_base = start_frame_index * channel_capacity_size;
    for (size_t i = 0; i < first_chunk_frames; ++i) {
        float* dst = view.interleaved_frames.data() + first_base + (i * channel_capacity_size);
        for (u32 ch = 0; ch < channel_capacity; ++ch) {
            float value = 0.0f;
            if (ch < input_channel_count)
                value = input[input_offset + ch];
            dst[ch] = value;
        }
        input_offset += input_channel_count;
    }

    for (size_t i = 0; i < second_chunk_frames; ++i) {
        float* dst = view.interleaved_frames.data() + (i * channel_capacity_size);
        for (u32 ch = 0; ch < channel_capacity; ++ch) {
            float value = 0.0f;
            if (ch < input_channel_count)
                value = input[input_offset + ch];
            dst[ch] = value;
        }
        input_offset += input_channel_count;
    }

    ring_stream_store_write_frame(header, write_frame + frames_to_write);
    return frames_to_write;
}

}
