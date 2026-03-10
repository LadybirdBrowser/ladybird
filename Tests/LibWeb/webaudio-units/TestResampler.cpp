/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Math.h>
#include <AK/Vector.h>
#include <LibTest/TestCase.h>
#include <LibWeb/WebAudio/Engine/SincResampler.h>

namespace Web::WebAudio::Render {

TEST_CASE(resampler_constant_signal_converges_to_unity)
{
    SampleRateConverter state;
    sample_rate_converter_init(state, 1, 1.0);

    constexpr size_t input_frames = 1024;
    Vector<f32> input;
    input.resize(input_frames);
    input.fill(1.0f);

    Vector<f32> output;
    output.resize(input_frames);
    output.fill(0.0f);

    Array<ReadonlySpan<f32>, 1> input_channels { input.span() };
    Array<Span<f32>, 1> output_channels { output.span() };

    auto result = sample_rate_converter_process(state, input_channels.span(), output_channels.span(), false);

    size_t const expected_produced = input_frames - (SincResamplerKernel::tap_count / 2);
    EXPECT_EQ(result.input_frames_consumed, input_frames);
    EXPECT_EQ(result.output_frames_produced, expected_produced);

    // Ignore the initial warmup region where the kernel spans pre-roll zeros.
    // After sufficient lookahead, a constant input should remain constant because the kernel is DC-normalized.
    size_t const check_start = 64;
    size_t const check_end = expected_produced > 64 ? expected_produced - 64 : 0;
    for (size_t i = check_start; i < check_end; ++i)
        EXPECT_APPROXIMATE_WITH_ERROR(output[i], 1.0f, 1e-4f);
}

TEST_CASE(resampler_two_channels_are_processed_in_lockstep)
{
    SampleRateConverter state;
    sample_rate_converter_init(state, 2, 1.0);

    constexpr size_t input_frames = 1024;
    Vector<f32> input_l;
    Vector<f32> input_r;
    input_l.resize(input_frames);
    input_r.resize(input_frames);
    input_l.fill(0.25f);
    input_r.fill(-0.75f);

    Vector<f32> output_l;
    Vector<f32> output_r;
    output_l.resize(input_frames);
    output_r.resize(input_frames);
    output_l.fill(0.0f);
    output_r.fill(0.0f);

    Array<ReadonlySpan<f32>, 2> input_channels { input_l.span(), input_r.span() };
    Array<Span<f32>, 2> output_channels { output_l.span(), output_r.span() };

    auto result = sample_rate_converter_process(state, input_channels.span(), output_channels.span(), false);

    size_t const expected_produced = input_frames - (SincResamplerKernel::tap_count / 2);
    EXPECT_EQ(result.input_frames_consumed, input_frames);
    EXPECT_EQ(result.output_frames_produced, expected_produced);

    size_t const check_start = 64;
    size_t const check_end = expected_produced > 64 ? expected_produced - 64 : 0;
    for (size_t i = check_start; i < check_end; ++i) {
        EXPECT_APPROXIMATE_WITH_ERROR(output_l[i], 0.25f, 1e-4f);
        EXPECT_APPROXIMATE_WITH_ERROR(output_r[i], -0.75f, 1e-4f);
    }
}

TEST_CASE(resampler_may_leave_input_unconsumed_when_output_buffer_is_limited)
{
    SampleRateConverter state;
    sample_rate_converter_init(state, 1, 1.0);

    // Provide more input than we can possibly consume when output capacity is small.
    // This models the situation where the rendering loop feeds quanta into a resampler, but the
    // output sink only accepts a bounded number of frames per call.
    constexpr size_t input_frames = 1024;
    constexpr size_t output_frames = 128;

    Vector<f32> input;
    input.resize(input_frames);
    input.fill(1.0f);

    Vector<f32> output;
    output.resize(output_frames);
    output.fill(0.0f);

    Array<ReadonlySpan<f32>, 1> input_channels { input.span() };
    Array<Span<f32>, 1> output_channels { output.span() };

    auto result = sample_rate_converter_process(state, input_channels.span(), output_channels.span(), false);

    EXPECT_EQ(result.output_frames_produced, output_frames);
    EXPECT(result.input_frames_consumed < input_frames);
    EXPECT(result.input_frames_consumed > 0u);
}

static f32 rms_of_signal(ReadonlySpan<f32> signal)
{
    if (signal.is_empty())
        return 0.0f;

    double sum_squares = 0.0;
    for (auto sample : signal)
        sum_squares += static_cast<double>(sample) * static_cast<double>(sample);
    return static_cast<f32>(AK::sqrt(sum_squares / static_cast<double>(signal.size())));
}

static f32 normalized_correlation(ReadonlySpan<f32> a, ReadonlySpan<f32> b)
{
    VERIFY(a.size() == b.size());
    if (a.is_empty())
        return 0.0f;

    double dot = 0.0;
    double aa = 0.0;
    double bb = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        double const x = static_cast<double>(a[i]);
        double const y = static_cast<double>(b[i]);
        dot += x * y;
        aa += x * x;
        bb += y * y;
    }

    double const denom = AK::sqrt(aa * bb);
    if (denom == 0.0 || __builtin_isnan(denom) || !__builtin_isfinite(denom))
        return 0.0f;
    return static_cast<f32>(dot / denom);
}

TEST_CASE(resampler_constant_signal_is_unity_across_common_ratios)
{
    // Sweep a mix of integral and fractional ratios, exercising both up- and down-sampling.
    // Ratio is input frames per output frame.
    constexpr Array<double, 7> ratios {
        0.5,
        44100.0 / 48000.0,
        1.0,
        48000.0 / 44100.0,
        2.0,
        3.0,
        4.0,
    };

    constexpr size_t input_frames = 16384;
    Vector<f32> input;
    input.resize(input_frames);
    input.fill(1.0f);

    Array<ReadonlySpan<f32>, 1> input_channels { input.span() };

    for (double ratio : ratios) {
        SampleRateConverter state;
        sample_rate_converter_init(state, 1, ratio);

        size_t const output_capacity = static_cast<size_t>(AK::ceil(static_cast<double>(input_frames) / ratio)) + 2048;
        Vector<f32> output;
        output.resize(output_capacity);
        output.fill(0.0f);

        Array<Span<f32>, 1> output_channels { output.span() };

        auto result = sample_rate_converter_process(state, input_channels.span(), output_channels.span(), false);
        EXPECT_EQ(result.input_frames_consumed, input_frames);
        EXPECT(result.output_frames_produced > 2048);

        // Ignore warmup/tail regions where the symmetric kernel spans implicit zeros.
        size_t const skip = 512;
        size_t const produced = result.output_frames_produced;
        size_t const begin = min(skip, produced);
        size_t const end = produced > skip ? produced - skip : begin;
        auto region = output.span().slice(begin, end - begin);

        f32 const out_rms = rms_of_signal(region);
        EXPECT(out_rms > 0.99f);
        EXPECT(out_rms < 1.01f);
    }
}

TEST_CASE(resampler_low_frequency_tone_tracks_across_fractional_ratios)
{
    // Ensure time mapping is consistent: for an input sine at f_in cycles/input-sample,
    // the output should be a sine at f_out = f_in * ratio cycles/output-sample.
    // Pick f_out constant across ratios so we can compare against a direct reference.
    constexpr double target_cycles_per_output_sample = 0.10;
    constexpr Array<double, 6> ratios {
        0.5,
        44100.0 / 48000.0,
        1.0,
        48000.0 / 44100.0,
        2.0,
        4.0,
    };

    constexpr size_t input_frames = 16384;
    Vector<f32> input;
    input.resize(input_frames);

    Array<ReadonlySpan<f32>, 1> input_channels { input.span() };

    for (double ratio : ratios) {
        SampleRateConverter state;
        sample_rate_converter_init(state, 1, ratio);

        double const cycles_per_input_sample = target_cycles_per_output_sample / ratio;
        for (size_t i = 0; i < input_frames; ++i) {
            double const phase = 2.0 * AK::Pi<double> * cycles_per_input_sample * static_cast<double>(i);
            input[i] = static_cast<f32>(AK::sin(phase));
        }

        size_t const output_capacity = static_cast<size_t>(AK::ceil(static_cast<double>(input_frames) / ratio)) + 2048;
        Vector<f32> output;
        output.resize(output_capacity);
        output.fill(0.0f);

        Array<Span<f32>, 1> output_channels { output.span() };

        auto result = sample_rate_converter_process(state, input_channels.span(), output_channels.span(), false);
        EXPECT_EQ(result.input_frames_consumed, input_frames);
        EXPECT(result.output_frames_produced > 2048);

        size_t const skip = 512;
        size_t const produced = result.output_frames_produced;
        size_t const begin = min(skip, produced);
        size_t const end = produced > skip ? produced - skip : begin;
        auto region = output.span().slice(begin, end - begin);

        // Phase-invariant correlation against a unit sinusoid at the expected output frequency.
        // The resampler is an FIR, so it introduces (frequency-dependent) phase delay.
        Vector<f32> reference_sin;
        Vector<f32> reference_cos;
        reference_sin.resize(region.size());
        reference_cos.resize(region.size());
        for (size_t i = 0; i < region.size(); ++i) {
            double const n = static_cast<double>(begin + i);
            double const angle = 2.0 * AK::Pi<double> * target_cycles_per_output_sample * n;
            reference_sin[i] = static_cast<f32>(AK::sin(angle));
            reference_cos[i] = static_cast<f32>(AK::cos(angle));
        }

        f32 const corr_sin = normalized_correlation(region, reference_sin.span());
        f32 const corr_cos = normalized_correlation(region, reference_cos.span());
        f32 const corr = static_cast<f32>(AK::sqrt((static_cast<double>(corr_sin) * corr_sin) + (static_cast<double>(corr_cos) * corr_cos)));
        EXPECT(corr > 0.98f);

        // RMS for a unit sine should be ~0.707.
        f32 const out_rms = rms_of_signal(region);
        EXPECT(out_rms > 0.60f);
        EXPECT(out_rms < 0.80f);
    }
}

TEST_CASE(resampler_downsampling_attenuates_above_cutoff)
{
    // This is a regression test for downsampling ratios where a ratio-independent kernel will
    // alias high-frequency content into the output band.
    //
    // We resample with ratio=2 (downsample by 2). The output Nyquist corresponds to 0.25 cycles
    // per input sample. A sine at 0.35 cycles/input-sample is above this and should be strongly
    // attenuated by the ratio-scaled low-pass kernel.

    constexpr double ratio = 2.0;
    SampleRateConverter state;
    sample_rate_converter_init(state, 1, ratio);

    constexpr size_t input_frames = 32768;
    Vector<f32> input;
    input.resize(input_frames);

    auto fill_sine = [&](double cycles_per_input_sample) {
        for (size_t i = 0; i < input_frames; ++i) {
            double const phase = 2.0 * AK::Pi<double> * cycles_per_input_sample * static_cast<double>(i);
            input[i] = static_cast<f32>(AK::sin(phase));
        }
    };

    Vector<f32> output;
    // Oversize the output buffer; we only measure the produced region.
    output.resize(input_frames);
    output.fill(0.0f);

    Array<ReadonlySpan<f32>, 1> input_channels { input.span() };
    Array<Span<f32>, 1> output_channels { output.span() };

    auto measure_rms_for_cycles = [&](double cycles_per_input_sample) -> f32 {
        sample_rate_converter_init(state, 1, ratio);
        fill_sine(cycles_per_input_sample);
        output.fill(0.0f);

        auto result = sample_rate_converter_process(state, input_channels.span(), output_channels.span(), false);
        EXPECT_EQ(result.input_frames_consumed, input_frames);
        EXPECT(result.output_frames_produced > 2048);

        // Ignore warmup/tail regions where the symmetric kernel spans implicit zeros.
        size_t const skip = 512;
        size_t const produced = result.output_frames_produced;
        size_t const begin = min(skip, produced);
        size_t const end = produced > skip ? produced - skip : begin;
        auto region = output.span().slice(begin, end - begin);
        return rms_of_signal(region);
    };

    // Low-frequency tone should pass with minimal attenuation.
    f32 const low_rms = measure_rms_for_cycles(0.10);
    EXPECT(low_rms > 0.60f);
    EXPECT(low_rms < 0.80f);

    // High-frequency tone above the downsampled Nyquist should be strongly attenuated.
    f32 const high_rms = measure_rms_for_cycles(0.35);
    EXPECT(high_rms < 0.10f);
}

}
