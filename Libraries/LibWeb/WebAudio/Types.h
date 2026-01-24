/*
 * Copyright (c) 2026, Ben Eidson <b.e.eidson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/Vector.h>

namespace Web::WebAudio {

// Stable identifier for AudioNode instances within a BaseAudioContext.
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u64, NodeID, CastToUnderlying);

struct AudioBus {
    explicit AudioBus(size_t channel_count, size_t frame_count)
        : m_channel_capacity(channel_count)
        , m_channel_count(channel_count)
        , m_frame_count(frame_count)
    {
        allocate_storage_and_zero();
    }

    explicit AudioBus(size_t channel_count, size_t frame_count, size_t channel_capacity)
        : m_channel_capacity(channel_capacity)
        , m_channel_count(channel_count)
        , m_frame_count(frame_count)
    {
        VERIFY(channel_count <= channel_capacity);
        allocate_storage_and_zero();
    }

    AudioBus(AudioBus const&) = delete;
    AudioBus& operator=(AudioBus const&) = delete;
    AudioBus(AudioBus&&) = default;
    AudioBus& operator=(AudioBus&&) = default;

    size_t channel_count() const { return m_channel_count; }
    size_t channel_capacity() const { return m_channel_capacity; }
    size_t frame_count() const { return m_frame_count; }

    Span<f32> channel(size_t channel_index)
    {
        VERIFY(channel_index < m_channel_count);
        return m_samples.span().slice(channel_index * m_frame_count, m_frame_count);
    }

    ReadonlySpan<f32> channel(size_t channel_index) const
    {
        VERIFY(channel_index < m_channel_count);
        return m_samples.span().slice(channel_index * m_frame_count, m_frame_count);
    }

    void set_channel_count(size_t channel_count)
    {
        VERIFY(channel_count <= m_channel_capacity);
        m_channel_count = channel_count;
    }

    void zero()
    {
        m_samples.span().slice(0, m_channel_count * m_frame_count).fill(0.0f);
    }

private:
    void allocate_storage_and_zero()
    {
        m_samples.resize(m_channel_capacity * m_frame_count);
        m_samples.fill(0.0f);
    }

    size_t m_channel_capacity { 0 };
    size_t m_channel_count { 0 };
    size_t m_frame_count { 0 };
    Vector<f32> m_samples;
};

}
