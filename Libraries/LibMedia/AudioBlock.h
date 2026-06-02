/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Checked.h>
#include <AK/FixedArray.h>
#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <AK/SaturatingMath.h>
#include <AK/Time.h>
#include <LibMedia/Audio/SampleSpecification.h>

namespace Media {

class AudioBlock {
public:
    Audio::SampleSpecification const& sample_specification() const { return m_sample_specification; }
    AK::Duration timestamp() const { return m_timestamp; }
    i64 timestamp_in_frames() const { return m_timestamp_in_frames; }
    i64 end_timestamp_in_frames() const { return saturating_add(m_timestamp_in_frames, AK::clamp_to<i64>(frame_count())); }
    AK::Duration end_timestamp() const { return AK::Duration::from_time_units(end_timestamp_in_frames(), 1, sample_rate()); }
    Span<float> channel_data(size_t channel)
    {
        VERIFY(channel < channel_count());
        return m_data.span().slice(channel * frame_capacity(), m_frame_count);
    }
    ReadonlySpan<float> channel_data(size_t channel) const
    {
        VERIFY(channel < channel_count());
        return m_data.span().slice(channel * frame_capacity(), m_frame_count);
    }
    float sample(size_t channel, size_t frame) const
    {
        return channel_data(channel)[frame];
    }
    void set_sample(size_t channel, size_t frame, float sample)
    {
        channel_data(channel)[frame] = sample;
    }

    void clear()
    {
        m_sample_specification = {};
        m_timestamp = {};
        m_timestamp_in_frames = 0;
        m_frame_count = 0;
    }
    void initialize(Audio::SampleSpecification sample_specification, AK::Duration timestamp, size_t frame_count)
    {
        VERIFY(sample_specification.is_valid());
        VERIFY(frame_count <= NumericLimits<i64>::max());
        VERIFY(!Checked<size_t>::multiplication_would_overflow(frame_count, sample_specification.channel_count()));
        m_sample_specification = sample_specification;
        m_timestamp = timestamp;
        m_timestamp_in_frames = timestamp.to_time_units(1, sample_rate());
        m_frame_count = frame_count;
        ensure_frame_capacity(frame_count);
    }
    void initialize(Audio::SampleSpecification sample_specification, i64 timestamp_in_frames, size_t frame_count)
    {
        VERIFY(sample_specification.is_valid());
        VERIFY(frame_count <= NumericLimits<i64>::max());
        VERIFY(!Checked<size_t>::multiplication_would_overflow(frame_count, sample_specification.channel_count()));
        m_sample_specification = sample_specification;
        m_timestamp_in_frames = timestamp_in_frames;
        m_timestamp = AK::Duration::from_time_units(timestamp_in_frames, 1, sample_rate());
        m_frame_count = frame_count;
        ensure_frame_capacity(frame_count);
    }
    void trim(size_t frame_count)
    {
        VERIFY(frame_count <= m_frame_count);
        m_frame_count = frame_count;
    }
    size_t copy_to_interleaved(Span<float> destination, size_t source_frame_offset = 0) const
    {
        VERIFY(!is_empty());
        auto channels = channel_count();
        VERIFY(destination.size() % channels == 0);

        auto available_frames = frame_count();
        if (source_frame_offset >= available_frames)
            return 0;

        auto frames_to_copy = min(destination.size() / channels, available_frames - source_frame_offset);
        for (size_t channel = 0; channel < channels; channel++) {
            auto source_channel = channel_data(channel).slice(source_frame_offset, frames_to_copy);
            for (size_t frame = 0; frame < frames_to_copy; frame++)
                destination[(frame * channels) + channel] = source_channel[frame];
        }
        return frames_to_copy * channels;
    }
    u32 sample_rate() const
    {
        return sample_specification().sample_rate();
    }
    void set_timestamp_in_frames(i64 timestamp_in_frames)
    {
        VERIFY(!is_empty());
        m_timestamp_in_frames = timestamp_in_frames;
        m_timestamp = AK::Duration::from_time_units(timestamp_in_frames, 1, sample_rate());
    }
    bool is_empty() const
    {
        return !sample_specification().is_valid();
    }
    size_t sample_count() const
    {
        return frame_count() * channel_count();
    }
    u8 channel_count() const
    {
        return sample_specification().channel_map().channel_count();
    }
    size_t frame_count() const
    {
        return m_frame_count;
    }

private:
    size_t frame_capacity() const
    {
        if (!is_empty())
            return m_data.size() / channel_count();
        return 0;
    }

    void ensure_frame_capacity(size_t frame_count)
    {
        if (frame_capacity() >= frame_count)
            return;
        VERIFY(!Checked<size_t>::multiplication_would_overflow(frame_count, channel_count()));
        m_data = MUST(FixedArray<float>::create(frame_count * channel_count()));
    }

    Audio::SampleSpecification m_sample_specification;
    AK::Duration m_timestamp;
    i64 m_timestamp_in_frames { 0 };
    size_t m_frame_count { 0 };
    FixedArray<float> m_data;
};

}
