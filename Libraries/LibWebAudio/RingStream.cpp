/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <AK/Types.h>
#include <LibWebAudio/RingStream.h>
#include <string.h>

namespace Web::WebAudio::Render {

using AudioServer::RingHeader;

ErrorOr<RingStreamView> validate_ring_stream_descriptor(RingStreamDescriptor& descriptor)
{
    if (descriptor.stream_id == 0)
        return Error::from_string_literal("RingStream: invalid stream id");

    if (!descriptor.shared_memory.is_valid())
        return Error::from_string_literal("RingStream: missing shared memory");

    if (descriptor.shared_memory.size() < sizeof(RingHeader))
        return Error::from_string_literal("RingStream: shared memory too small for header");

    RingHeader* header = descriptor.shared_memory.data<RingHeader>();
    if (!header)
        return Error::from_string_literal("RingStream: shared memory header mapping failed");

    if (header->capacity_frames == 0 || header->channel_capacity == 0)
        return Error::from_string_literal("RingStream: invalid capacity");

    if (header->channel_count == 0)
        return Error::from_string_literal("RingStream: invalid channel count");

    if (header->channel_count > header->channel_capacity)
        return Error::from_string_literal("RingStream: channel_count exceeds channel_capacity");

    if (descriptor.format.sample_rate_hz != 0 && descriptor.format.sample_rate_hz != header->sample_rate_hz)
        return Error::from_string_literal("RingStream: descriptor sample rate does not match shared header");
    if (descriptor.format.channel_count != 0 && descriptor.format.channel_count != header->channel_count)
        return Error::from_string_literal("RingStream: descriptor channel count does not match shared header");
    if (descriptor.format.channel_capacity != 0 && descriptor.format.channel_capacity != header->channel_capacity)
        return Error::from_string_literal("RingStream: descriptor channel capacity does not match shared header");
    if (descriptor.format.capacity_frames != 0 && descriptor.format.capacity_frames != header->capacity_frames)
        return Error::from_string_literal("RingStream: descriptor capacity does not match shared header");

    size_t const data_bytes = static_cast<size_t>(header->channel_capacity) * static_cast<size_t>(header->capacity_frames) * sizeof(f32);
    size_t const required_bytes = sizeof(RingHeader) + data_bytes;
    if (descriptor.shared_memory.size() < required_bytes)
        return Error::from_string_literal("RingStream: shared memory too small for ring data");

    u8* base = descriptor.shared_memory.data<u8>();
    if (!base)
        return Error::from_string_literal("RingStream: shared memory base mapping failed");

    auto* data_f32 = reinterpret_cast<f32*>(base + sizeof(RingHeader));
    Span<f32> data { data_f32, data_bytes / sizeof(f32) };

    return RingStreamView { .header = header, .interleaved_frames = data };
}

size_t ring_stream_pop_planar_from_read_frame(RingStreamView view, u64 read_frame, size_t frames_to_read, Span<Span<f32>> out_channels, u32 expected_channel_count)
{
    if (!view.header)
        return 0;
    if (frames_to_read == 0 || expected_channel_count == 0)
        return 0;
    if (out_channels.size() < expected_channel_count)
        return 0;

    RingHeader& header = *view.header;

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

RingStreamConsumer::RingStreamConsumer(RingStreamView view)
    : m_view(view)
{
}

RingHeader& RingStreamConsumer::header() const
{
    return *m_view.header;
}

RingStreamPeekResult RingStreamConsumer::peek_with_timing() const
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

RingStreamPopResult RingStreamConsumer::try_pop_planar_with_timing(Span<Span<f32>> out_channels, size_t requested_frames, u32 expected_channel_count) const
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

size_t RingStreamConsumer::try_pop_planar(Span<Span<f32>> out_channels, size_t requested_frames, u32 expected_channel_count) const
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

size_t RingStreamConsumer::skip_frames(size_t requested_frames) const
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

RingStreamProducer::RingStreamProducer(RingStreamView view)
    : m_view(view)
{
}

RingHeader& RingStreamProducer::header() const
{
    return *m_view.header;
}

void RingStreamProducer::initialize_format(u32 sample_rate_hz, u32 channel_count, u32 channel_capacity, u64 capacity_frames) const
{
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
}

size_t RingStreamProducer::try_push_interleaved(ReadonlySpan<f32> interleaved_samples, u32 input_channel_count) const
{
    if (input_channel_count == 0)
        return 0;

    size_t input_frame_count = interleaved_samples.size() / input_channel_count;
    if (input_frame_count == 0)
        return 0;

    u64 write_frame = ring_stream_load_write_frame(header());

    u64 const capacity_frames = header().capacity_frames;
    u32 const channel_capacity = header().channel_capacity;

    if (capacity_frames == 0 || channel_capacity == 0)
        return 0;

    size_t frames_to_copy = input_frame_count;
    size_t input_frame_offset = 0;
    u64 write_advance = static_cast<u64>(input_frame_count);
    u64 effective_write_frame = write_frame;

    if (frames_to_copy > static_cast<size_t>(capacity_frames)) {
        frames_to_copy = static_cast<size_t>(capacity_frames);
        input_frame_offset = input_frame_count - frames_to_copy;
    }
    effective_write_frame = write_frame + static_cast<u64>(input_frame_offset);

    if (frames_to_copy == 0)
        return 0;

    u32 channels_to_copy = channel_capacity < input_channel_count ? channel_capacity : input_channel_count;
    bool can_memcpy_whole_frames = (channels_to_copy == channel_capacity) && (input_channel_count == channel_capacity);

    size_t const start_frame_index = static_cast<size_t>(effective_write_frame % capacity_frames);
    size_t const first_chunk_frames = min(frames_to_copy, static_cast<size_t>(capacity_frames - start_frame_index));
    size_t const second_chunk_frames = frames_to_copy - first_chunk_frames;

    auto push_chunk = [&](size_t chunk_frame_index, size_t chunk_frames, size_t input_chunk_offset) {
        if (chunk_frames == 0)
            return;

        f32* dst_base = m_view.interleaved_frames.data() + (chunk_frame_index * static_cast<size_t>(channel_capacity));
        size_t const dst_samples = chunk_frames * static_cast<size_t>(channel_capacity);
        __builtin_memset(dst_base, 0, dst_samples * sizeof(f32));

        f32 const* src_base = interleaved_samples.data() + (input_chunk_offset * static_cast<size_t>(input_channel_count));
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

void RingStreamProducer::set_timeline_for_start(u32 timeline_sample_rate, u64 media_start_frame, u64 ring_start_frame) const
{
    AK::atomic_store(&header().timeline_media_start_frame, media_start_frame, AK::MemoryOrder::memory_order_relaxed);
    AK::atomic_store(&header().timeline_media_start_at_ring_frame, ring_start_frame, AK::MemoryOrder::memory_order_relaxed);
    AK::atomic_store(&header().timeline_sample_rate, timeline_sample_rate, AK::MemoryOrder::memory_order_relaxed);
    (void)AK::atomic_fetch_add(&header().timeline_generation, static_cast<u64>(1), AK::MemoryOrder::memory_order_release);
}

}
