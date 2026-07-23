/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Math.h>
#include <LibWeb/WebAudio/Rendering/AudioBus.h>

namespace Web::WebAudio::Rendering {

// https://webaudio.github.io/web-audio-api/#ChannelLayouts
// Speakers channel layouts exist for mono, stereo, quad and 5.1; all other channel counts use discrete mixing.
enum SpeakerChannel : u8 {
    Left = 0,
    Right = 1,
    Center = 2,
    LFE = 3,
    SurroundLeft = 4,
    SurroundRight = 5,
};

AudioBus::AudioBus(size_t channel_count, size_t frame_count)
    : m_frame_count(frame_count)
{
    set_channel_count(channel_count);
}

void AudioBus::zero()
{
    for (auto& channel : m_channels)
        channel.span().fill(0.f);
}

bool AudioBus::is_silent() const
{
    return !any_of(m_channels, [](auto const& channel) {
        return any_of(channel, [](auto sample) { return sample != 0.f; });
    });
}

// Changes the number of channels; sample data is not preserved.
void AudioBus::set_channel_count(size_t channel_count)
{
    if (m_channels.size() == channel_count)
        return;
    m_channels.resize(channel_count);
    for (auto& channel : m_channels)
        channel.resize(m_frame_count);
}

void AudioBus::copy_from(AudioBus const& source)
{
    VERIFY(source.frame_count() == frame_count());
    set_channel_count(source.channel_count());
    for (size_t channel_index = 0; channel_index < m_channels.size(); ++channel_index)
        m_channels[channel_index] = source.m_channels[channel_index];
}

// https://webaudio.github.io/web-audio-api/#channel-up-mixing-and-down-mixing
void AudioBus::sum_from(AudioBus const& source, Bindings::ChannelInterpretation channel_interpretation)
{
    VERIFY(source.frame_count() == frame_count());
    if (channel_interpretation == Bindings::ChannelInterpretation::Speakers)
        sum_from_speakers(source);
    else
        sum_from_discrete(source);
}

// https://webaudio.github.io/web-audio-api/#UpMix-sub and https://webaudio.github.io/web-audio-api/#down-mix
void AudioBus::sum_from_speakers(AudioBus const& source)
{
    auto source_channels = source.channel_count();
    auto destination_channels = channel_count();

    auto sum_channel = [&](size_t destination_index, size_t source_index, float gain = 1.f) {
        auto destination_span = channel(destination_index);
        auto source_span = source.channel(source_index);
        for (size_t frame = 0; frame < m_frame_count; ++frame)
            destination_span[frame] += source_span[frame] * gain;
    };

    if (source_channels == destination_channels) {
        for (size_t channel_index = 0; channel_index < source_channels; ++channel_index)
            sum_channel(channel_index, channel_index);
        return;
    }

    // NB: Up-mixing: mono/stereo/quad sources into stereo, quad or 5.1 destinations.
    if (source_channels == 1 && destination_channels == 2) {
        // 1 -> 2: output.L = input, output.R = input
        sum_channel(Left, 0);
        sum_channel(Right, 0);
        return;
    }
    if (source_channels == 1 && destination_channels == 4) {
        // 1 -> 4: output.L = input, output.R = input, output.SL = 0, output.SR = 0
        sum_channel(Left, 0);
        sum_channel(Right, 0);
        return;
    }
    if (source_channels == 1 && destination_channels == 6) {
        // 1 -> 5.1: output.C = input, all other channels silent
        sum_channel(Center, 0);
        return;
    }
    if (source_channels == 2 && destination_channels == 4) {
        // 2 -> 4: output.L = input.L, output.R = input.R, surround channels silent
        sum_channel(Left, Left);
        sum_channel(Right, Right);
        return;
    }
    if (source_channels == 2 && destination_channels == 6) {
        // 2 -> 5.1: output.L = input.L, output.R = input.R, all other channels silent
        sum_channel(Left, Left);
        sum_channel(Right, Right);
        return;
    }
    if (source_channels == 4 && destination_channels == 6) {
        // 4 -> 5.1: output.L = input.L, output.R = input.R, output.SL = input.SL, output.SR = input.SR
        sum_channel(Left, Left);
        sum_channel(Right, Right);
        sum_channel(SurroundLeft, 2);
        sum_channel(SurroundRight, 3);
        return;
    }

    // NB: Down-mixing: stereo/quad/5.1 sources into mono, stereo or quad destinations.
    if (source_channels == 2 && destination_channels == 1) {
        // 2 -> 1: output = 0.5 * (input.L + input.R)
        sum_channel(0, Left, 0.5f);
        sum_channel(0, Right, 0.5f);
        return;
    }
    if (source_channels == 4 && destination_channels == 1) {
        // 4 -> 1: output = 0.25 * (input.L + input.R + input.SL + input.SR)
        for (size_t channel_index = 0; channel_index < 4; ++channel_index)
            sum_channel(0, channel_index, 0.25f);
        return;
    }
    if (source_channels == 6 && destination_channels == 1) {
        // 5.1 -> 1: output = sqrt(0.5) * (input.L + input.R) + input.C + 0.5 * (input.SL + input.SR)
        sum_channel(0, Left, AK::Sqrt1_2<float>);
        sum_channel(0, Right, AK::Sqrt1_2<float>);
        sum_channel(0, Center);
        sum_channel(0, SurroundLeft, 0.5f);
        sum_channel(0, SurroundRight, 0.5f);
        return;
    }
    if (source_channels == 4 && destination_channels == 2) {
        // 4 -> 2: output.L = 0.5 * (input.L + input.SL), output.R = 0.5 * (input.R + input.SR)
        sum_channel(Left, Left, 0.5f);
        sum_channel(Left, 2, 0.5f);
        sum_channel(Right, Right, 0.5f);
        sum_channel(Right, 3, 0.5f);
        return;
    }
    if (source_channels == 6 && destination_channels == 2) {
        // 5.1 -> 2: output.L = input.L + sqrt(0.5) * (input.C + input.SL),
        //           output.R = input.R + sqrt(0.5) * (input.C + input.SR)
        sum_channel(Left, Left);
        sum_channel(Left, Center, AK::Sqrt1_2<float>);
        sum_channel(Left, SurroundLeft, AK::Sqrt1_2<float>);
        sum_channel(Right, Right);
        sum_channel(Right, Center, AK::Sqrt1_2<float>);
        sum_channel(Right, SurroundRight, AK::Sqrt1_2<float>);
        return;
    }
    if (source_channels == 6 && destination_channels == 4) {
        // 5.1 -> 4: output.L = input.L + sqrt(0.5) * input.C, output.R = input.R + sqrt(0.5) * input.C,
        //           output.SL = input.SL, output.SR = input.SR
        sum_channel(Left, Left);
        sum_channel(Left, Center, AK::Sqrt1_2<float>);
        sum_channel(Right, Right);
        sum_channel(Right, Center, AK::Sqrt1_2<float>);
        sum_channel(2, SurroundLeft);
        sum_channel(3, SurroundRight);
        return;
    }

    // NB: All other layout combinations fall back to discrete mixing.
    sum_from_discrete(source);
}

// https://webaudio.github.io/web-audio-api/#dom-channelinterpretation-discrete
void AudioBus::sum_from_discrete(AudioBus const& source)
{
    // Up-mix by filling channels until they run out then zero out remaining channels. Down-mix by filling as many
    // channels as possible, then dropping remaining channels.
    auto channels_to_sum = min(source.channel_count(), channel_count());
    for (size_t channel_index = 0; channel_index < channels_to_sum; ++channel_index) {
        auto destination_span = channel(channel_index);
        auto source_span = source.channel(channel_index);
        for (size_t frame = 0; frame < m_frame_count; ++frame)
            destination_span[frame] += source_span[frame];
    }
}

}
