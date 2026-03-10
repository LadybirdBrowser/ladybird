/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <LibWebAudio/LibWebAudio.h>

namespace Web::WebAudio::Render {

using AudioServer::RingHeader;

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

ErrorOr<RingStreamView> validate_ring_stream_descriptor(RingStreamDescriptor& descriptor);

size_t ring_stream_pop_planar_from_read_frame(RingStreamView view, u64 read_frame, size_t frames_to_read, Span<Span<f32>> out_channels, u32 expected_channel_count);

class RingStreamConsumer {
public:
    explicit RingStreamConsumer(RingStreamView view);

    RingHeader& header() const;
    RingStreamPeekResult peek_with_timing() const;
    RingStreamPopResult try_pop_planar_with_timing(Span<Span<f32>> out_channels, size_t requested_frames, u32 expected_channel_count) const;
    size_t try_pop_planar(Span<Span<f32>> out_channels, size_t requested_frames, u32 expected_channel_count) const;
    size_t skip_frames(size_t requested_frames) const;

private:
    mutable RingStreamView m_view;
};

class RingStreamProducer {
public:
    explicit RingStreamProducer(RingStreamView view);

    RingHeader& header() const;
    void initialize_format(u32 sample_rate_hz, u32 channel_count, u32 channel_capacity, u64 capacity_frames) const;
    size_t try_push_interleaved(ReadonlySpan<f32> interleaved_samples, u32 input_channel_count) const;
    void set_timeline_for_start(u32 timeline_sample_rate, u64 media_start_frame, u64 ring_start_frame) const;

private:
    mutable RingStreamView m_view;
};

}
