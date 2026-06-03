/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FixedArray.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/AudioBlock.h>

namespace Audio {

class AudioBuffer {
public:
    explicit AudioBuffer(SampleSpecification);

    size_t frame_count() const { return m_frame_count; }

    void clear();
    void append(Media::AudioBlock const&);
    void append_silence(size_t frame_count);
    void drop_front(size_t frame_count);
    void copy_frames_to(size_t source_offset, size_t frame_count, size_t destination_offset, Media::AudioBlock&) const;

private:
    void ensure_capacity(size_t required_frame_capacity);
    void copy_channel_to_buffer(ReadonlySpan<float>, size_t channel, size_t destination_offset);
    void copy_channel_from_buffer(size_t channel, size_t source_offset, Span<float>) const;
    void zero_channel(size_t channel, size_t destination_offset, size_t frame_count);

    SampleSpecification const m_sample_specification;
    FixedArray<float> m_data;
    size_t m_frame_capacity { 0 };
    size_t m_frame_count { 0 };
    size_t m_start_offset { 0 };
};

}
