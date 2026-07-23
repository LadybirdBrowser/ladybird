/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibWeb/Bindings/AudioNode.h>
#include <LibWeb/Export.h>

namespace Web::WebAudio::Rendering {

// A block of audio samples for a single render quantum, holding one buffer of frames per channel.
class WEB_API AudioBus {
public:
    AudioBus(size_t channel_count, size_t frame_count);

    size_t channel_count() const { return m_channels.size(); }
    size_t frame_count() const { return m_frame_count; }

    Span<float> channel(size_t index) { return m_channels[index]; }
    ReadonlySpan<float> channel(size_t index) const { return m_channels[index]; }

    void zero();
    bool is_silent() const;
    void set_channel_count(size_t);
    void copy_from(AudioBus const&);

    // https://webaudio.github.io/web-audio-api/#channel-up-mixing-and-down-mixing
    void sum_from(AudioBus const& source, Bindings::ChannelInterpretation);

private:
    void sum_from_speakers(AudioBus const& source);
    void sum_from_discrete(AudioBus const& source);

    Vector<Vector<float>> m_channels;
    size_t m_frame_count { 0 };
};

}
