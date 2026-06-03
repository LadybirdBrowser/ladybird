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
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/AudioBlockTiming.h>

namespace Media {

class AudioBlock {
public:
    Audio::SampleSpecification const& sample_specification() const { return m_sample_specification; }
    AudioBlockTiming const& timing() const { return m_timing; }
    AudioBlockTiming& timing() { return m_timing; }
    AK::Duration media_time_start() const { return m_timing.media_time_start(); }
    AK::Duration media_time_duration() const { return m_timing.media_time_duration(); }
    AK::Duration media_time_end() const { return m_timing.media_time_end(); }
    i64 first_frame_index() const { return m_timing.first_frame_index(); }
    i64 end_frame_index() const { return m_timing.end_frame_index(); }
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
        m_timing.clear();
        m_frame_count = 0;
    }
    void initialize(Audio::SampleSpecification sample_specification, AK::Duration media_time_start, size_t frame_count)
    {
        VERIFY(sample_specification.is_valid());
        VERIFY(frame_count <= NumericLimits<i64>::max());
        VERIFY(!Checked<size_t>::multiplication_would_overflow(frame_count, sample_specification.channel_count()));
        m_sample_specification = sample_specification;
        m_frame_count = frame_count;
        m_timing.initialize(media_time_start, sample_rate(), AK::clamp_to<i64>(frame_count));
        ensure_frame_capacity(frame_count);
    }
    void initialize(Audio::SampleSpecification sample_specification, i64 first_frame_index, size_t frame_count)
    {
        VERIFY(sample_specification.is_valid());
        VERIFY(frame_count <= NumericLimits<i64>::max());
        VERIFY(!Checked<size_t>::multiplication_would_overflow(frame_count, sample_specification.channel_count()));
        m_sample_specification = sample_specification;
        m_frame_count = frame_count;
        m_timing.initialize(first_frame_index, sample_rate(), AK::clamp_to<i64>(frame_count));
        ensure_frame_capacity(frame_count);
    }
    void trim(size_t frame_count)
    {
        VERIFY(frame_count <= m_frame_count);
        m_frame_count = frame_count;
        m_timing.set_frame_count(sample_rate(), AK::clamp_to<i64>(frame_count));
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
    void set_first_frame_index(i64 first_frame_index)
    {
        VERIFY(!is_empty());
        m_timing.set_first_frame_index(first_frame_index, sample_rate());
    }
    void set_media_time_start(AK::Duration media_time_start)
    {
        VERIFY(!is_empty());
        m_timing.set_media_time_start(media_time_start);
    }
    void set_media_time_duration(AK::Duration media_time_duration)
    {
        VERIFY(!is_empty());
        m_timing.set_media_time_duration(media_time_duration);
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
    AudioBlockTiming m_timing;
    size_t m_frame_count { 0 };
    FixedArray<float> m_data;
};

}
