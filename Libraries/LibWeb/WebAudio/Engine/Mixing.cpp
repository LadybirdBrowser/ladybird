/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/WebAudio/Engine/Mixing.h>

namespace Web::WebAudio::Render {

static void accumulate_discrete(AudioBus& output, AudioBus const& input);

// https://webaudio.github.io/web-audio-api/#channel-up-mixing-and-down-mixing
void mix_inputs_into(AudioBus& output, ReadonlySpan<AudioBus const*> inputs)
{
    // These are the speakers mixing equations for basic layouts (mono, stereo, quad, 5.1).
    // If either side is not one of these, sum or replicate the samples.

    // Channel ordering for the basic layouts:
    // mono: [M]
    // stereo: [L, R]
    // quad: [L, R, SL, SR]
    // 5.1: [L, R, C, LFE, SL, SR]

    output.zero();
    if (inputs.is_empty())
        return;

    size_t const output_frames = output.frame_count();
    size_t const output_channels = output.channel_count();
    if (output_channels == 0)
        return;

    auto is_basic_speaker_layout = [](size_t channels) {
        return channels == 1 || channels == 2 || channels == 4 || channels == 6;
    };

    auto accumulate_speaker_mix = [&](AudioBus const& input) {
        size_t const input_channels = input.channel_count();
        size_t const frames = min(output_frames, input.frame_count());

        if (input_channels == output_channels) {
            accumulate_discrete(output, input);
            return;
        }

        if (input_channels == 1 && output_channels == 2) {
            auto in_m = input.channel(0);
            auto out_l = output.channel(0);
            auto out_r = output.channel(1);
            for (size_t i = 0; i < frames; ++i) {
                out_l[i] += in_m[i];
                out_r[i] += in_m[i];
            }
            return;
        }
        if (input_channels == 1 && output_channels == 4) {
            auto in_m = input.channel(0);
            auto out_l = output.channel(0);
            auto out_r = output.channel(1);
            for (size_t i = 0; i < frames; ++i) {
                out_l[i] += in_m[i];
                out_r[i] += in_m[i];
            }
            return;
        }
        if (input_channels == 1 && output_channels == 6) {
            auto in_m = input.channel(0);
            auto out_c = output.channel(2);
            for (size_t i = 0; i < frames; ++i)
                out_c[i] += in_m[i];
            return;
        }
        if (input_channels == 2 && output_channels == 4) {
            accumulate_discrete(output, input);
            return;
        }
        if (input_channels == 2 && output_channels == 6) {
            accumulate_discrete(output, input);
            return;
        }
        if (input_channels == 4 && output_channels == 6) {
            auto out_l = output.channel(0);
            auto out_r = output.channel(1);
            auto out_sl = output.channel(4);
            auto out_sr = output.channel(5);
            auto in_l = input.channel(0);
            auto in_r = input.channel(1);
            auto in_sl = input.channel(2);
            auto in_sr = input.channel(3);
            for (size_t i = 0; i < frames; ++i) {
                out_l[i] += in_l[i];
                out_r[i] += in_r[i];
                out_sl[i] += in_sl[i];
                out_sr[i] += in_sr[i];
            }
            return;
        }

        if (input_channels == 2 && output_channels == 1) {
            auto in_l = input.channel(0);
            auto in_r = input.channel(1);
            auto out_m = output.channel(0);
            for (size_t i = 0; i < frames; ++i)
                out_m[i] += 0.5f * (in_l[i] + in_r[i]);
            return;
        }
        if (input_channels == 4 && output_channels == 1) {
            auto in_l = input.channel(0);
            auto in_r = input.channel(1);
            auto in_sl = input.channel(2);
            auto in_sr = input.channel(3);
            auto out_m = output.channel(0);
            for (size_t i = 0; i < frames; ++i)
                out_m[i] += 0.25f * (in_l[i] + in_r[i] + in_sl[i] + in_sr[i]);
            return;
        }
        if (input_channels == 6 && output_channels == 1) {
            auto const sqrt_half = AK::sqrt(0.5f);
            auto in_l = input.channel(0);
            auto in_r = input.channel(1);
            auto in_c = input.channel(2);
            auto in_sl = input.channel(4);
            auto in_sr = input.channel(5);
            auto out_m = output.channel(0);
            for (size_t i = 0; i < frames; ++i)
                out_m[i] += (sqrt_half * (in_l[i] + in_r[i])) + in_c[i] + (0.5f * (in_sl[i] + in_sr[i]));
            return;
        }
        if (input_channels == 4 && output_channels == 2) {
            auto in_l = input.channel(0);
            auto in_r = input.channel(1);
            auto in_sl = input.channel(2);
            auto in_sr = input.channel(3);
            auto out_l = output.channel(0);
            auto out_r = output.channel(1);
            for (size_t i = 0; i < frames; ++i) {
                out_l[i] += 0.5f * (in_l[i] + in_sl[i]);
                out_r[i] += 0.5f * (in_r[i] + in_sr[i]);
            }
            return;
        }
        if (input_channels == 6 && output_channels == 2) {
            f32 const sqrt_half = AK::sqrt(0.5f);
            auto in_l = input.channel(0);
            auto in_r = input.channel(1);
            auto in_c = input.channel(2);
            auto in_sl = input.channel(4);
            auto in_sr = input.channel(5);
            auto out_l = output.channel(0);
            auto out_r = output.channel(1);
            for (size_t i = 0; i < frames; ++i) {
                out_l[i] += in_l[i] + (sqrt_half * (in_c[i] + in_sl[i]));
                out_r[i] += in_r[i] + (sqrt_half * (in_c[i] + in_sr[i]));
            }
            return;
        }
        if (input_channels == 6 && output_channels == 4) {
            f32 const sqrt_half = AK::sqrt(0.5f);
            auto in_l = input.channel(0);
            auto in_r = input.channel(1);
            auto in_c = input.channel(2);
            auto in_sl = input.channel(4);
            auto in_sr = input.channel(5);
            auto out_l = output.channel(0);
            auto out_r = output.channel(1);
            auto out_sl = output.channel(2);
            auto out_sr = output.channel(3);
            for (size_t i = 0; i < frames; ++i) {
                out_l[i] += in_l[i] + (sqrt_half * in_c[i]);
                out_r[i] += in_r[i] + (sqrt_half * in_c[i]);
                out_sl[i] += in_sl[i];
                out_sr[i] += in_sr[i];
            }
            return;
        }

        // Any other specialized channel mappings.
        accumulate_discrete(output, input);
    };

    for (AudioBus const* bus : inputs) {
        if (!bus)
            continue;
        size_t const input_channels = bus->channel_count();
        if (input_channels == 0)
            continue;

        if (is_basic_speaker_layout(output_channels) && is_basic_speaker_layout(input_channels)) {
            accumulate_speaker_mix(*bus);
            continue;
        }

        accumulate_discrete(output, *bus);
    }
}

void mix_inputs_discrete_into(AudioBus& output, ReadonlySpan<AudioBus const*> inputs)
{
    output.zero();
    if (inputs.is_empty())
        return;
    size_t const output_channels = output.channel_count();
    if (output_channels == 0)
        return;

    for (auto const* bus : inputs) {
        if (!bus)
            continue;
        if (bus->channel_count() == 0)
            continue;

        accumulate_discrete(output, *bus);
    }
}

void copy_planar_to_interleaved(Span<ReadonlySpan<f32>> input_channels, Span<f32> out_interleaved, size_t frame_count)
{
    size_t const channel_count = input_channels.size();
    if (channel_count == 0 || frame_count == 0)
        return;

    VERIFY(out_interleaved.size() >= channel_count * frame_count);
    for (size_t ch = 0; ch < channel_count; ++ch)
        VERIFY(input_channels[ch].size() >= frame_count);

    if (channel_count == 1) {
        input_channels[0].slice(0, frame_count).copy_to(out_interleaved.trim(frame_count));
        return;
    }

    if (channel_count == 2) {
        auto in_l = input_channels[0].slice(0, frame_count);
        auto in_r = input_channels[1].slice(0, frame_count);
        for (size_t frame = 0; frame < frame_count; ++frame) {
            size_t const base = frame * 2;
            out_interleaved[base + 0] = in_l[frame];
            out_interleaved[base + 1] = in_r[frame];
        }
        return;
    }

    for (size_t frame = 0; frame < frame_count; ++frame) {
        size_t const base = frame * channel_count;
        for (size_t ch = 0; ch < channel_count; ++ch)
            out_interleaved[base + ch] = input_channels[ch][frame];
    }
}

static void accumulate_discrete(AudioBus& output, AudioBus const& input)
{
    size_t const frames = min(output.frame_count(), input.frame_count());
    size_t const channels_to_copy = min(output.channel_count(), input.channel_count());
    for (size_t ch = 0; ch < channels_to_copy; ++ch) {
        auto out = output.channel(ch);
        auto in = input.channel(ch);
        for (size_t i = 0; i < frames; ++i)
            out[i] += in[i];
    }
}

}
