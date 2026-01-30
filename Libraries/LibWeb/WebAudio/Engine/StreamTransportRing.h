/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <LibWeb/WebAudio/Engine/StreamTransport.h>
#include <string.h>

namespace Web::WebAudio::Render {

struct RingStreamPeekResult {
    size_t available_frames { 0 };
    Optional<AK::Duration> start_time;
    u64 timeline_generation { 0 };
};

struct RingStreamPopResult {
    size_t frames_read { 0 };
    Optional<AK::Duration> start_time;
    u64 timeline_generation { 0 };
};

// Copies interleaved frames out of the ring into planar output and advances the ring read cursor.
// Preconditions:
// - view.header is non-null
// - view.interleaved_frames is sized for header().channel_capacity * header().capacity_frames
// - out_channels.size() >= expected_channel_count
// - each out_channels[ch].size() >= frames_to_read
inline size_t ring_stream_pop_planar_from_read_frame(RingStreamView view, u64 read_frame, size_t frames_to_read, Span<Span<f32>> out_channels, u32 expected_channel_count)
{
    if (!view.header)
        return 0;
    if (frames_to_read == 0 || expected_channel_count == 0)
        return 0;
    if (out_channels.size() < expected_channel_count)
        return 0;

    RingStreamHeader& header = *view.header;

    u32 const channels_to_copy = header.channel_capacity < expected_channel_count ? header.channel_capacity : expected_channel_count;
    if (channels_to_copy < expected_channel_count) {
        for (u32 ch = channels_to_copy; ch < expected_channel_count; ++ch)
            out_channels[ch].slice(0, frames_to_read).fill(0.0f);
    }

    u64 const capacity_frames = header.capacity_frames;
    u32 const channel_capacity = header.channel_capacity;
    if (capacity_frames == 0 || channel_capacity == 0)
        return 0;

    size_t const start_frame_index = static_cast<size_t>(read_frame % capacity_frames);
    size_t const first_chunk_frames = min(frames_to_read, static_cast<size_t>(capacity_frames - start_frame_index));
    size_t const second_chunk_frames = frames_to_read - first_chunk_frames;

    size_t const first_chunk_base = start_frame_index * static_cast<size_t>(channel_capacity);

    for (u32 ch = 0; ch < channels_to_copy; ++ch) {
        f32 const* src = view.interleaved_frames.data() + first_chunk_base + ch;
        f32* dst = out_channels[ch].data();
        for (size_t i = 0; i < first_chunk_frames; ++i)
            dst[i] = src[i * static_cast<size_t>(channel_capacity)];

        if (second_chunk_frames > 0) {
            f32 const* src2 = view.interleaved_frames.data() + ch;
            for (size_t i = 0; i < second_chunk_frames; ++i)
                dst[first_chunk_frames + i] = src2[i * static_cast<size_t>(channel_capacity)];
        }
    }

    ring_stream_store_read_frame(header, read_frame + frames_to_read);
    return frames_to_read;
}

// RingStreamConsumer provides non-blocking SPSC reads from a shared ring.
// It performs overrun detection and advances the read cursor.
class RingStreamConsumer {
public:
    explicit RingStreamConsumer(RingStreamView view)
        : m_view(view)
    {
    }

    RingStreamHeader& header() const { return *m_view.header; }

    RingStreamPeekResult peek_with_timing() const
    {
        RingStreamPeekResult result;

        result.timeline_generation = AK::atomic_load(&header().timeline_generation, AK::MemoryOrder::memory_order_acquire);

        u64 read_frame = ring_stream_load_read_frame(header());
        u64 write_frame = ring_stream_load_write_frame(header());

        (void)ring_stream_consumer_detect_and_fix_overrun(header(), read_frame, write_frame);

        result.available_frames = ring_stream_available_frames(header(), read_frame, write_frame);

        u32 const timeline_sample_rate = AK::atomic_load(&header().timeline_sample_rate, AK::MemoryOrder::memory_order_relaxed);
        if (timeline_sample_rate != 0 && result.available_frames > 0) {
            u64 const timeline_media_start_frame = AK::atomic_load(&header().timeline_media_start_frame, AK::MemoryOrder::memory_order_relaxed);
            u64 const timeline_media_start_at_ring_frame = AK::atomic_load(&header().timeline_media_start_at_ring_frame, AK::MemoryOrder::memory_order_relaxed);
            u64 const media_frame_at_read = timeline_media_start_frame + (read_frame - timeline_media_start_at_ring_frame);
            u64 const clamped = media_frame_at_read < static_cast<u64>(AK::NumericLimits<i64>::max()) ? media_frame_at_read : static_cast<u64>(AK::NumericLimits<i64>::max());
            result.start_time = AK::Duration::from_time_units(static_cast<i64>(clamped), 1, timeline_sample_rate);
        }

        return result;
    }

    RingStreamPopResult try_pop_planar_with_timing(Span<Span<f32>> out_channels, size_t requested_frames, u32 expected_channel_count) const
    {
        RingStreamPopResult result;

        if (requested_frames == 0 || expected_channel_count == 0)
            return result;

        if (out_channels.size() < expected_channel_count)
            return result;

        for (u32 ch = 0; ch < expected_channel_count; ++ch) {
            if (out_channels[ch].size() < requested_frames)
                return result;
        }

        result.timeline_generation = AK::atomic_load(&header().timeline_generation, AK::MemoryOrder::memory_order_acquire);

        u64 read_frame = ring_stream_load_read_frame(header());
        u64 write_frame = ring_stream_load_write_frame(header());

        (void)ring_stream_consumer_detect_and_fix_overrun(header(), read_frame, write_frame);

        size_t const available = ring_stream_available_frames(header(), read_frame, write_frame);
        size_t const frames_to_read = available < requested_frames ? available : requested_frames;
        if (frames_to_read == 0)
            return result;

        u32 const timeline_sample_rate = AK::atomic_load(&header().timeline_sample_rate, AK::MemoryOrder::memory_order_relaxed);
        if (timeline_sample_rate != 0) {
            u64 const timeline_media_start_frame = AK::atomic_load(&header().timeline_media_start_frame, AK::MemoryOrder::memory_order_relaxed);
            u64 const timeline_media_start_at_ring_frame = AK::atomic_load(&header().timeline_media_start_at_ring_frame, AK::MemoryOrder::memory_order_relaxed);
            u64 const media_frame_at_read = timeline_media_start_frame + (read_frame - timeline_media_start_at_ring_frame);
            u64 const clamped = media_frame_at_read < static_cast<u64>(AK::NumericLimits<i64>::max()) ? media_frame_at_read : static_cast<u64>(AK::NumericLimits<i64>::max());
            result.start_time = AK::Duration::from_time_units(static_cast<i64>(clamped), 1, timeline_sample_rate);
        }

        result.frames_read = ring_stream_pop_planar_from_read_frame(m_view, read_frame, frames_to_read, out_channels, expected_channel_count);
        return result;
    }

    // Pop planar samples into out_channels.
    // out_channels.size() must be >= expected_channel_count.
    // Each output span must have size >= requested_frames.
    size_t try_pop_planar(Span<Span<f32>> out_channels, size_t requested_frames, u32 expected_channel_count) const
    {
        if (requested_frames == 0 || expected_channel_count == 0)
            return 0;

        if (out_channels.size() < expected_channel_count)
            return 0;

        for (u32 ch = 0; ch < expected_channel_count; ++ch) {
            if (out_channels[ch].size() < requested_frames)
                return 0;
        }

        u64 read_frame = ring_stream_load_read_frame(header());
        u64 write_frame = ring_stream_load_write_frame(header());

        (void)ring_stream_consumer_detect_and_fix_overrun(header(), read_frame, write_frame);

        size_t const available = ring_stream_available_frames(header(), read_frame, write_frame);
        size_t const frames_to_read = available < requested_frames ? available : requested_frames;
        if (frames_to_read == 0)
            return 0;

        return ring_stream_pop_planar_from_read_frame(m_view, read_frame, frames_to_read, out_channels, expected_channel_count);
    }

    // Advances the read cursor without copying any samples. Returns frames skipped.
    size_t skip_frames(size_t requested_frames) const
    {
        if (requested_frames == 0)
            return 0;

        u64 read_frame = ring_stream_load_read_frame(header());
        u64 write_frame = ring_stream_load_write_frame(header());

        (void)ring_stream_consumer_detect_and_fix_overrun(header(), read_frame, write_frame);

        size_t const available = ring_stream_available_frames(header(), read_frame, write_frame);
        size_t const frames_to_skip = available < requested_frames ? available : requested_frames;
        if (frames_to_skip == 0)
            return 0;

        ring_stream_store_read_frame(header(), read_frame + frames_to_skip);
        return frames_to_skip;
    }

private:
    mutable RingStreamView m_view;
};

// RingStreamProducer provides non-blocking SPSC writes into a shared ring.
// No allocations; callers decide whether to drop or wait based on return values.
class RingStreamProducer {
public:
    explicit RingStreamProducer(RingStreamView view, StreamOverflowPolicy overflow_policy)
        : m_view(view)
        , m_overflow_policy(overflow_policy)
    {
    }

    RingStreamHeader& header() const { return *m_view.header; }

    // Initialize format and reset cursors. Intended for the creator of the shared memory.
    void initialize_format(u32 sample_rate_hz, u32 channel_count, u32 channel_capacity, u64 capacity_frames) const
    {
        header().version = ring_stream_version;
        header().sample_rate_hz = sample_rate_hz;
        header().channel_count = channel_count;
        header().channel_capacity = channel_capacity;
        header().capacity_frames = capacity_frames;

        ring_stream_store_read_frame(header(), 0);
        ring_stream_store_write_frame(header(), 0);

        header().overrun_frames_total = 0;
        AK::atomic_store(&header().timeline_generation, static_cast<u64>(1), AK::MemoryOrder::memory_order_release);
        AK::atomic_store(&header().timeline_sample_rate, 0u, AK::MemoryOrder::memory_order_relaxed);
        AK::atomic_store(&header().timeline_media_start_frame, static_cast<u64>(0), AK::MemoryOrder::memory_order_relaxed);
        AK::atomic_store(&header().timeline_media_start_at_ring_frame, static_cast<u64>(0), AK::MemoryOrder::memory_order_relaxed);
        ring_stream_clear_producer_timestamp_anchor(header());
    }

    // Returns frames written.
    size_t try_push_interleaved(ReadonlySpan<f32> interleaved_samples, u32 input_channel_count) const
    {
        if (input_channel_count == 0)
            return 0;

        size_t input_frame_count = interleaved_samples.size() / input_channel_count;
        if (input_frame_count == 0)
            return 0;

        u64 read_frame = ring_stream_load_read_frame(header());
        u64 write_frame = ring_stream_load_write_frame(header());

        u64 const capacity_frames = header().capacity_frames;
        u32 const channel_capacity = header().channel_capacity;

        if (capacity_frames == 0 || channel_capacity == 0)
            return 0;

        u64 unread = write_frame > read_frame ? (write_frame - read_frame) : 0;
        u64 used = unread < capacity_frames ? unread : capacity_frames;
        u64 available_to_write = capacity_frames - used;

        size_t frames_to_copy = input_frame_count;
        size_t input_frame_offset = 0;
        u64 write_advance = static_cast<u64>(input_frame_count);
        u64 effective_write_frame = write_frame;

        if (m_overflow_policy == StreamOverflowPolicy::DropOldest) {
            // When a producer pushes more than the ring can hold, preserve the newest frames
            // while keeping the monotonic write cursor progressing by the full input size.
            if (frames_to_copy > static_cast<size_t>(capacity_frames)) {
                frames_to_copy = static_cast<size_t>(capacity_frames);
                input_frame_offset = input_frame_count - frames_to_copy;
            }
            effective_write_frame = write_frame + static_cast<u64>(input_frame_offset);
        } else {
            u64 cap = available_to_write;
            frames_to_copy = frames_to_copy < cap ? frames_to_copy : static_cast<size_t>(cap);
            write_advance = static_cast<u64>(frames_to_copy);
        }

        if (frames_to_copy == 0)
            return 0;

        u32 channels_to_copy = channel_capacity < input_channel_count ? channel_capacity : input_channel_count;
        bool can_memcpy_whole_frames = (channels_to_copy == channel_capacity) && (input_channel_count == channel_capacity);

        size_t const start_frame_index = static_cast<size_t>(effective_write_frame % capacity_frames);
        size_t const first_chunk_frames = min(frames_to_copy, static_cast<size_t>(capacity_frames - start_frame_index));
        size_t const second_chunk_frames = frames_to_copy - first_chunk_frames;

        auto push_chunk = [&](size_t chunk_frame_index, size_t chunk_frames, size_t input_frame_offset) {
            if (chunk_frames == 0)
                return;

            f32* dst_base = m_view.interleaved_frames.data() + (chunk_frame_index * static_cast<size_t>(channel_capacity));
            size_t const dst_samples = chunk_frames * static_cast<size_t>(channel_capacity);
            __builtin_memset(dst_base, 0, dst_samples * sizeof(f32));

            f32 const* src_base = interleaved_samples.data() + (input_frame_offset * static_cast<size_t>(input_channel_count));
            if (can_memcpy_whole_frames) {
                __builtin_memcpy(dst_base, src_base, chunk_frames * static_cast<size_t>(channel_capacity) * sizeof(f32));
                return;
            }

            size_t const src_stride = static_cast<size_t>(input_channel_count);
            size_t const dst_stride = static_cast<size_t>(channel_capacity);
            size_t const copy_bytes = static_cast<size_t>(channels_to_copy) * sizeof(f32);
            for (size_t i = 0; i < chunk_frames; ++i)
                __builtin_memcpy(dst_base + (i * dst_stride), src_base + (i * src_stride), copy_bytes);
        };

        push_chunk(start_frame_index, first_chunk_frames, input_frame_offset);
        push_chunk(0, second_chunk_frames, input_frame_offset + first_chunk_frames);

        ring_stream_store_write_frame(header(), write_frame + write_advance);
        return static_cast<size_t>(write_advance);
    }

    void set_timeline_for_start(u32 timeline_sample_rate, u64 media_start_frame, u64 ring_start_frame) const
    {
        AK::atomic_store(&header().timeline_media_start_frame, media_start_frame, AK::MemoryOrder::memory_order_relaxed);
        AK::atomic_store(&header().timeline_media_start_at_ring_frame, ring_start_frame, AK::MemoryOrder::memory_order_relaxed);
        AK::atomic_store(&header().timeline_sample_rate, timeline_sample_rate, AK::MemoryOrder::memory_order_relaxed);
        (void)AK::atomic_fetch_add(&header().timeline_generation, static_cast<u64>(1), AK::MemoryOrder::memory_order_release);
    }

private:
    mutable RingStreamView m_view;
    StreamOverflowPolicy m_overflow_policy { StreamOverflowPolicy::DropOldest };
};

}
