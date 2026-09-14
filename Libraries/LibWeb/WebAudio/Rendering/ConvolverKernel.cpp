/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/WebAudio/Rendering/ConvolverKernel.h>
#include <LibWeb/WebAudio/Rendering/FFT.h>

namespace Web::WebAudio::Rendering {

// https://webaudio.github.io/web-audio-api/#dom-convolvernode-normalize
static float calculate_normalization_scale(AudioBufferContents const& contents)
{
    static constexpr double gain_calibration = 0.00125;
    static constexpr double gain_calibration_sample_rate = 44100;
    static constexpr double min_power = 0.000125;

    // Normalize by RMS power.
    double power = 0;
    for (auto const& samples : contents.channels) {
        for (auto sample : samples)
            power += static_cast<double>(sample) * sample;
    }
    power = AK::sqrt(power / (contents.channels.size() * contents.length()));

    // Protect against accidental overload.
    if (!__builtin_isfinite(power) || power < min_power)
        power = min_power;

    auto scale = 1 / power;

    // Calibrate to make perceived volume same as unprocessed.
    scale *= gain_calibration;

    // Scale depends on sample-rate.
    if (contents.sample_rate != 0)
        scale *= gain_calibration_sample_rate / contents.sample_rate;

    // True-stereo compensation.
    if (contents.channels.size() == 4)
        scale *= 0.5;

    return static_cast<float>(scale);
}

RefPtr<ConvolverKernel> ConvolverKernel::create(AudioBufferContents const& contents, size_t partition_size, bool normalize)
{
    auto length = contents.length();
    if (length == 0)
        return nullptr;

    auto scale = normalize ? calculate_normalization_scale(contents) : 1.f;

    // Each partition is zero-padded to twice its length so that the cyclic convolution of the transform produces the
    // linear convolution the rendering thread is after.
    auto transform_size = 2 * partition_size;
    auto bin_count = partition_size + 1;
    auto partition_count = length > partition_size ? ceil_div(length - partition_size, partition_size) : 0;

    FFT fft { transform_size };
    Vector<float> real;
    Vector<float> imag;
    real.resize(transform_size);
    imag.resize(transform_size);

    Vector<Channel> channels;
    channels.ensure_capacity(contents.channels.size());
    for (auto const& samples : contents.channels) {
        Channel channel;
        channel.head.resize(partition_size);
        channel.real.resize(partition_count * bin_count);
        channel.imag.resize(partition_count * bin_count);

        for (size_t frame = 0; frame < min(partition_size, length); ++frame)
            channel.head[frame] = samples[frame] * scale;

        for (size_t partition = 0; partition < partition_count; ++partition) {
            auto offset = (partition + 1) * partition_size;
            auto frames_in_partition = min(partition_size, length - offset);
            for (size_t frame = 0; frame < frames_in_partition; ++frame)
                real[frame] = samples[offset + frame] * scale;
            for (size_t frame = frames_in_partition; frame < transform_size; ++frame)
                real[frame] = 0;
            for (size_t frame = 0; frame < transform_size; ++frame)
                imag[frame] = 0;

            fft.transform(real, imag);

            for (size_t bin = 0; bin < bin_count; ++bin) {
                channel.real[partition * bin_count + bin] = real[bin];
                channel.imag[partition * bin_count + bin] = imag[bin];
            }
        }

        channels.unchecked_append(move(channel));
    }

    return adopt_ref(*new ConvolverKernel(move(channels), partition_count, bin_count));
}

NonnullRefPtr<ConvolverDelayLine> ConvolverDelayLine::create(ConvolverKernel const& kernel)
{
    auto length = kernel.partition_count() + 1;

    Array<FixedArray<float>, MAX_CHANNEL_COUNT> real;
    Array<FixedArray<float>, MAX_CHANNEL_COUNT> imag;
    for (size_t channel = 0; channel < MAX_CHANNEL_COUNT; ++channel) {
        real[channel] = MUST(FixedArray<float>::create(length * kernel.bin_count()));
        imag[channel] = MUST(FixedArray<float>::create(length * kernel.bin_count()));
    }

    return adopt_ref(*new ConvolverDelayLine(length, move(real), move(imag)));
}

}
