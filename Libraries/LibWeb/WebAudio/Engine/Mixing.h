/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/Types.h>

namespace Web::WebAudio::Render {

class AudioBus;

// https://webaudio.github.io/web-audio-api/#channel-up-mixing-and-down-mixing
// Allocation-free as long as output has sufficient preallocated channels.
WEB_API void mix_inputs_into(AudioBus& output, ReadonlySpan<AudioBus const*> inputs);

// Discrete mixing: sum channels 1:1 without upmix/downmix.
// - If an input has fewer channels than output, missing channels are treated as silence.
// - If an input has more channels than output, extra channels are ignored.
// This is useful for nodes that conceptually operate on discrete channels (e.g. ChannelSplitter).
WEB_API void mix_inputs_discrete_into(AudioBus& output, ReadonlySpan<AudioBus const*> inputs);

// Copy planar channel data into an interleaved output buffer.
// - input_channels.size() determines the output channel count.
// - Each input channel span must have size >= frame_count.
// - out_interleaved must have size >= input_channels.size() * frame_count.
WEB_API void copy_planar_to_interleaved(Span<ReadonlySpan<f32>> input_channels, Span<f32> out_interleaved, size_t frame_count);

class AudioBus {
public:
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

    [[nodiscard]] AudioBus clone_with_new_channel_capacity(size_t new_channel_capacity) const
    {
        return clone_resized(m_channel_count, m_frame_count, new_channel_capacity);
    }

    [[nodiscard]] AudioBus clone_resized(size_t new_channel_count, size_t new_frame_count, size_t new_channel_capacity) const
    {
        AudioBus cloned(new_channel_count, new_frame_count, new_channel_capacity);

        size_t const channels_to_copy = AK::min(m_channel_count, new_channel_count);
        size_t const frames_to_copy = AK::min(m_frame_count, new_frame_count);
        for (size_t ch = 0; ch < channels_to_copy; ++ch) {
            channel(ch).slice(0, frames_to_copy).copy_to(cloned.channel(ch).slice(0, frames_to_copy));
        }
        return cloned;
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
