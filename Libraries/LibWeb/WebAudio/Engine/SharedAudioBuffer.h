/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibWeb/WebAudio/Types.h>

namespace Web::WebAudio::Render {

class SharedAudioBuffer final : public RefCounted<SharedAudioBuffer> {
public:
    static NonnullRefPtr<SharedAudioBuffer> create(f32 sample_rate, size_t channel_count, size_t length_in_sample_frames, Vector<Vector<f32>> channels)
    {
        return adopt_ref(*new SharedAudioBuffer(sample_rate, channel_count, length_in_sample_frames, move(channels)));
    }

    static NonnullRefPtr<SharedAudioBuffer> create_from_planar(f32 sample_rate, Vector<Vector<f32>> const& channels)
    {
        size_t channel_count = channels.size();
        size_t length = 0;
        for (auto const& ch : channels)
            length = max(length, ch.size());

        Vector<Vector<f32>> padded;
        padded.resize(channel_count);
        for (size_t ch = 0; ch < channel_count; ++ch) {
            padded[ch] = channels[ch];
            if (padded[ch].size() < length)
                padded[ch].resize(length, 0.0f);
        }

        return create(sample_rate, channel_count, length, move(padded));
    }

    f32 sample_rate() const { return m_sample_rate; }
    size_t channel_count() const { return m_channel_count; }
    size_t length_in_sample_frames() const { return m_length_in_sample_frames; }

    ReadonlySpan<f32> channel(size_t index) const
    {
        if (index >= m_channels.size())
            return {};
        return m_channels[index].span();
    }

private:
    SharedAudioBuffer(f32 sample_rate, size_t channel_count, size_t length_in_sample_frames, Vector<Vector<f32>> channels)
        : m_sample_rate(sample_rate)
        , m_channel_count(channel_count)
        , m_length_in_sample_frames(length_in_sample_frames)
        , m_channels(move(channels))
    {
    }

    f32 m_sample_rate { 0.0f };
    size_t m_channel_count { 0 };
    size_t m_length_in_sample_frames { 0 };
    Vector<Vector<f32>> m_channels;
};

}
