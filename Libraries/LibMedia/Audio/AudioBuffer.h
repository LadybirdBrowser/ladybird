/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FixedArray.h>
#include <LibMedia/Audio/SampleSpecification.h>

namespace Audio {

class AudioBuffer {
public:
    AudioBuffer() = default;

    AudioBuffer(SampleSpecification sample_specification, size_t frame_count)
        : m_sample_specification(sample_specification)
        , m_frame_count(frame_count)
        , m_data(MUST(FixedArray<float>::create(frame_count * sample_specification.channel_count())))
    {
    }

    SampleSpecification const& sample_specification() const { return m_sample_specification; }
    u8 channel_count() const { return m_sample_specification.channel_count(); }
    size_t frame_count() const { return m_frame_count; }

    Span<float> channel_data(size_t channel)
    {
        VERIFY(channel < channel_count());
        return m_data.span().slice(channel * m_frame_count, m_frame_count);
    }

    ReadonlySpan<float> channel_data(size_t channel) const
    {
        VERIFY(channel < channel_count());
        return m_data.span().slice(channel * m_frame_count, m_frame_count);
    }

    void zero()
    {
        m_data.fill_with(0.0f);
    }

    void zero_frames(size_t starting_frame, size_t frame_count)
    {
        VERIFY(starting_frame <= m_frame_count);
        VERIFY(frame_count <= m_frame_count - starting_frame);
        for (size_t channel = 0; channel < channel_count(); channel++) {
            for (auto& sample : channel_data(channel).slice(starting_frame, frame_count))
                sample = 0.0f;
        }
    }

private:
    SampleSpecification m_sample_specification;
    size_t m_frame_count { 0 };
    FixedArray<float> m_data;
};

}
