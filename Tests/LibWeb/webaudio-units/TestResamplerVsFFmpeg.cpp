/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Format.h>
#include <AK/Math.h>
#include <AK/Vector.h>
#include <LibCore/ElapsedTimer.h>
#include <LibTest/TestCase.h>
#include <LibWeb/WebAudio/Engine/SincResampler.h>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace Web::WebAudio::Render {

static f32 rms_of_signal(ReadonlySpan<f32> signal)
{
    if (signal.is_empty())
        return 0.0f;

    double sum_squares = 0.0;
    for (auto sample : signal)
        sum_squares += static_cast<double>(sample) * static_cast<double>(sample);
    return static_cast<f32>(AK::sqrt(sum_squares / static_cast<double>(signal.size())));
}

static double normalized_correlation(ReadonlySpan<f32> a, ReadonlySpan<f32> b)
{
    VERIFY(a.size() == b.size());
    if (a.is_empty())
        return 0.0;

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
        return 0.0;
    return dot / denom;
}

struct FFmpegResampler {
    SwrContext* ctx { nullptr };
    int in_rate { 0 };
    int out_rate { 0 };
    size_t channel_count { 0 };
    Vector<f32> output;
};

static ErrorOr<FFmpegResampler> create_ffmpeg_resampler(size_t channel_count, size_t input_frames_per_call, int in_rate, int out_rate)
{
    // Interleaved float.
    AVChannelLayout in_layout {};
    av_channel_layout_default(&in_layout, static_cast<int>(channel_count));
    AVChannelLayout out_layout {};
    av_channel_layout_default(&out_layout, static_cast<int>(channel_count));

    SwrContext* swr = nullptr;

    int ret = swr_alloc_set_opts2(
        &swr,
        &out_layout,
        AV_SAMPLE_FMT_FLT,
        out_rate,
        &in_layout,
        AV_SAMPLE_FMT_FLT,
        in_rate,
        0,
        nullptr);

    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&out_layout);

    if (ret < 0 || !swr)
        return Error::from_string_literal("swr_alloc_set_opts2 failed");

    // Use FFmpeg defaults (quality is controlled by swr options; we intentionally do not tweak them).
    ret = swr_init(swr);
    if (ret < 0) {
        swr_free(&swr);
        return Error::from_string_literal("swr_init failed");
    }

    // Compute a conservative output size.
    // See: swr_get_delay() + av_rescale_rnd() pattern from FFmpeg docs.
    // Allocate a conservative output buffer for one convert() call.
    // Streaming converts can carry internal delay across calls, so include some fixed headroom.
    int64_t const delay = swr_get_delay(swr, in_rate);
    int64_t const out_capacity_frames = av_rescale_rnd(delay + static_cast<int64_t>(input_frames_per_call), out_rate, in_rate, AV_ROUND_UP) + 8192;

    FFmpegResampler resampler;
    resampler.ctx = swr;
    resampler.in_rate = in_rate;
    resampler.out_rate = out_rate;
    resampler.channel_count = channel_count;
    resampler.output.resize(static_cast<size_t>(max<int64_t>(0, out_capacity_frames)) * channel_count);
    resampler.output.fill(0.0f);
    return resampler;
}

static ErrorOr<size_t> process_ffmpeg_chunk(FFmpegResampler& resampler, ReadonlySpan<f32> input)
{
    if (!resampler.ctx)
        return Error::from_string_literal("FFmpegResampler has no context");

    VERIFY(resampler.channel_count > 0);
    VERIFY(input.size() % resampler.channel_count == 0);
    size_t const input_frames = input.size() / resampler.channel_count;

    auto const* in_ptr = reinterpret_cast<u8 const*>(input.data());
    u8 const* in_data[1] { in_ptr };

    auto* out_ptr = reinterpret_cast<u8*>(resampler.output.data());
    u8* out_data[1] { out_ptr };

    // swr_convert counts are in frames (samples per channel), not total scalar floats.
    int produced = swr_convert(resampler.ctx, out_data, static_cast<int>(resampler.output.size() / resampler.channel_count), in_data, static_cast<int>(input_frames));
    if (produced < 0)
        return Error::from_string_literal("swr_convert failed");

    return static_cast<size_t>(produced);
}

static ResampleResult process_ladybird_chunk(SampleRateConverter& state, ReadonlySpan<f32> input, Span<f32> output)
{
    Array<ReadonlySpan<f32>, 1> input_channels { input };
    Array<Span<f32>, 1> output_channels { output };

    // Streaming mode: do not flush between chunks.
    return sample_rate_converter_process(state, input_channels.span(), output_channels.span(), false);
}

static ResampleResult process_ladybird_chunk_stereo(SampleRateConverter& state, ReadonlySpan<f32> input_l, ReadonlySpan<f32> input_r, Span<f32> output_l, Span<f32> output_r)
{
    Array<ReadonlySpan<f32>, 2> input_channels { input_l, input_r };
    Array<Span<f32>, 2> output_channels { output_l, output_r };
    return sample_rate_converter_process(state, input_channels.span(), output_channels.span(), false);
}

static double best_alignment_correlation(ReadonlySpan<f32> a, ReadonlySpan<f32> b, size_t base, size_t window, i64 max_shift, i64& best_shift)
{
    best_shift = 0;
    double best = -1.0;

    if (a.size() < base + window || b.size() < base + window)
        return best;

    for (i64 shift = -max_shift; shift <= max_shift; ++shift) {
        i64 const a_start = static_cast<i64>(base);
        i64 const b_start = static_cast<i64>(base) + shift;
        if (b_start < 0)
            continue;

        size_t const a0 = static_cast<size_t>(a_start);
        size_t const b0 = static_cast<size_t>(b_start);
        if (a0 + window > a.size() || b0 + window > b.size())
            continue;

        auto aw = a.slice(a0, window);
        auto bw = b.slice(b0, window);
        double const c = normalized_correlation(aw, bw);
        if (c > best) {
            best = c;
            best_shift = shift;
        }
    }

    return best;
}

TEST_CASE(webaudio_resampler_matches_ffmpeg_for_common_rates)
{
    // This is opt in via WEBAUDIO_STRESS_TESTS and compares the output and performance
    // of our windowed-sinc SRC against FFmpeg/libswresample.

    struct RatePair {
        int in_rate;
        int out_rate;
    };

    constexpr Array<RatePair, 30> pairs {
        // Music/common desktop rates.
        RatePair { 44100, 48000 },
        RatePair { 48000, 44100 },
        RatePair { 48000, 96000 },
        RatePair { 96000, 48000 },
        RatePair { 44100, 96000 },
        RatePair { 96000, 44100 },

        // Voice/telephony-ish rates.
        RatePair { 8000, 16000 },
        RatePair { 16000, 8000 },
        RatePair { 8000, 48000 },
        RatePair { 48000, 8000 },
        RatePair { 8000, 44100 },
        RatePair { 44100, 8000 },
        RatePair { 8000, 96000 },
        RatePair { 96000, 8000 },

        RatePair { 12000, 48000 },
        RatePair { 48000, 12000 },

        RatePair { 16000, 48000 },
        RatePair { 48000, 16000 },
        RatePair { 16000, 44100 },
        RatePair { 44100, 16000 },
        RatePair { 16000, 96000 },
        RatePair { 96000, 16000 },

        // Other common historic rates.
        RatePair { 11025, 44100 },
        RatePair { 44100, 11025 },
        RatePair { 22050, 44100 },
        RatePair { 44100, 22050 },
        RatePair { 24000, 48000 },
        RatePair { 48000, 24000 },
        RatePair { 32000, 48000 },
        RatePair { 48000, 32000 },
    };

    // A smaller stereo subset so we can quickly validate stereo optimizations.
    constexpr Array<RatePair, 10> stereo_pairs {
        RatePair { 44100, 48000 },
        RatePair { 48000, 44100 },
        RatePair { 48000, 96000 },
        RatePair { 96000, 48000 },
        RatePair { 44100, 96000 },
        RatePair { 96000, 44100 },
        RatePair { 48000, 22050 },
        RatePair { 22050, 48000 },
        RatePair { 44100, 32000 },
        RatePair { 32000, 44100 },
    };

    // Keep runtime bounded by scaling input length with the input rate.
    // (This also keeps a consistent time duration across rate pairs.)
    constexpr double duration_seconds = 2.0;
    constexpr size_t iterations = 20;
    Vector<f32> input;
    Vector<f32> input_l;
    Vector<f32> input_r;
    Vector<f32> input_interleaved;

    i64 total_ladybird_us = 0;
    i64 total_ffmpeg_us = 0;

    auto run_case = [&](RatePair pair, size_t channel_count) {
        size_t const input_frames_per_call = max<size_t>(4096, static_cast<size_t>(static_cast<double>(pair.in_rate) * duration_seconds));
        input.resize(input_frames_per_call);

        // Construct a sum of sines.
        // Frequencies are in cycles per input sample.
        // For downsampling (ratio > 1), keep these below the tightened cutoff (0.5/ratio).
        double const ratio = static_cast<double>(pair.in_rate) / static_cast<double>(pair.out_rate);
        double const lowpass_scale = ratio > 1.0 ? (1.0 / ratio) : 1.0;
        double const cutoff_cycles_per_input_sample = 0.5 * lowpass_scale;
        Array<double, 3> const cycles_per_input_sample {
            0.20 * cutoff_cycles_per_input_sample,
            0.35 * cutoff_cycles_per_input_sample,
            0.45 * cutoff_cycles_per_input_sample,
        };

        auto synth = [&](Vector<f32>& out, double phase_offset_cycles) {
            out.resize(input_frames_per_call);
            for (size_t i = 0; i < input_frames_per_call; ++i) {
                double value = 0.0;
                for (auto f : cycles_per_input_sample) {
                    double const angle = 2.0 * AK::Pi<double> * (f * static_cast<double>(i) + phase_offset_cycles);
                    value += AK::sin(angle);
                }
                value *= (1.0 / static_cast<double>(cycles_per_input_sample.size()));
                out[i] = static_cast<f32>(value);
            }
        };

        if (channel_count == 1) {
            synth(input, 0.0);
        } else {
            synth(input_l, 0.0);
            synth(input_r, 0.125);
            input_interleaved.resize(input_frames_per_call * channel_count);
            for (size_t i = 0; i < input_frames_per_call; ++i) {
                input_interleaved[(i * channel_count) + 0] = input_l[i];
                input_interleaved[(i * channel_count) + 1] = input_r[i];
            }
        }

        // Init/alloc once per pair, then measure process-only across iterations.
        // Init/alloc once per pair, then measure steady-state streaming across iterations.
        auto ffmpeg_resampler_or_error = create_ffmpeg_resampler(channel_count, input_frames_per_call, pair.in_rate, pair.out_rate);
        EXPECT(!ffmpeg_resampler_or_error.is_error());
        auto ffmpeg_resampler = ffmpeg_resampler_or_error.release_value();

        // Ratio is input frames per output frame.
        double const resample_ratio = static_cast<double>(pair.in_rate) / static_cast<double>(pair.out_rate);
        SampleRateConverter ladybird_state;
        sample_rate_converter_init(ladybird_state, channel_count, resample_ratio);

        size_t const ladybird_output_capacity = static_cast<size_t>(AK::ceil(static_cast<double>(input_frames_per_call) / resample_ratio)) + 8192;
        Vector<f32> ladybird_output_storage;
        Vector<f32> ladybird_output_l;
        Vector<f32> ladybird_output_r;
        if (channel_count == 1) {
            ladybird_output_storage.resize(ladybird_output_capacity);
        } else {
            ladybird_output_l.resize(ladybird_output_capacity);
            ladybird_output_r.resize(ladybird_output_capacity);
        }

        i64 ffmpeg_us = 0;
        size_t ffmpeg_produced = 0;
        {
            auto timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
            for (size_t it = 0; it < iterations; ++it) {
                auto produced_or_error = process_ffmpeg_chunk(ffmpeg_resampler, channel_count == 1 ? input.span() : input_interleaved.span());
                EXPECT(!produced_or_error.is_error());
                ffmpeg_produced = produced_or_error.release_value();
            }
            ffmpeg_us = timer.elapsed_time().to_microseconds();
        }

        i64 ladybird_us = 0;
        ResampleResult ladybird_result;
        {
            auto timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
            for (size_t it = 0; it < iterations; ++it) {
                if (channel_count == 1)
                    ladybird_result = process_ladybird_chunk(ladybird_state, input.span(), ladybird_output_storage.span());
                else
                    ladybird_result = process_ladybird_chunk_stereo(ladybird_state, input_l.span(), input_r.span(), ladybird_output_l.span(), ladybird_output_r.span());
            }
            ladybird_us = timer.elapsed_time().to_microseconds();
        }

        total_ladybird_us += ladybird_us;
        total_ffmpeg_us += ffmpeg_us;

        EXPECT_EQ(ladybird_result.input_frames_consumed, input_frames_per_call);

        auto check_channel = [&](ReadonlySpan<f32> ladybird_out, size_t channel_index, i64& best_shift_out) {
            // FFmpeg output is interleaved.
            size_t const ffmpeg_total_scalars = min(ffmpeg_resampler.output.size(), ffmpeg_produced * channel_count);
            size_t const ffmpeg_frames = ffmpeg_total_scalars / channel_count;
            size_t const min_frames = min(ladybird_out.size(), ffmpeg_frames);
            EXPECT(min_frames > 2048);

            Vector<f32> ffmpeg_deinterleaved;
            ffmpeg_deinterleaved.resize(min_frames);
            for (size_t i = 0; i < min_frames; ++i)
                ffmpeg_deinterleaved[i] = ffmpeg_resampler.output[(i * channel_count) + channel_index];

            // Choose a dynamic comparison region so low output rates (e.g. 8kHz) still have enough samples.
            size_t const base = min<size_t>(512, min_frames / 8);
            size_t window = min<size_t>(8192, (min_frames - base) / 2);
            window = max<size_t>(512, window);

            // Align outputs.
            i64 best_shift = 0;
            double const corr = best_alignment_correlation(ladybird_out.slice(0, min_frames), ffmpeg_deinterleaved.span(), base, window, 8192, best_shift);
            EXPECT(corr > 0.98);
            best_shift_out = best_shift;

            // Compute RMS error on the aligned comparison window.
            i64 const b_start_i = static_cast<i64>(base) + best_shift;
            EXPECT(b_start_i >= 0);

            size_t const a0 = base;
            size_t const b0 = static_cast<size_t>(b_start_i);
            EXPECT(a0 + window <= min_frames);
            EXPECT(b0 + window <= min_frames);

            auto a = ladybird_out.slice(a0, window);
            auto b = ffmpeg_deinterleaved.span().slice(b0, window);

            // Allow a best-fit gain between implementations.
            double dot_ab = 0.0;
            double dot_bb = 0.0;
            for (size_t i = 0; i < window; ++i) {
                double const av = static_cast<double>(a[i]);
                double const bv = static_cast<double>(b[i]);
                dot_ab += av * bv;
                dot_bb += bv * bv;
            }

            double gain = 1.0;
            if (dot_bb != 0.0 && !__builtin_isnan(dot_bb) && __builtin_isfinite(dot_bb))
                gain = dot_ab / dot_bb;

            double sum_sq = 0.0;
            for (size_t i = 0; i < window; ++i) {
                double const d = static_cast<double>(a[i]) - (gain * static_cast<double>(b[i]));
                sum_sq += d * d;
            }
            f32 const rmse = static_cast<f32>(AK::sqrt(sum_sq / static_cast<double>(window)));

            // Loose threshold: filters differ, but in-band content should be very similar.
            EXPECT(rmse < 0.02f);

            // Also sanity-check that we didn't trivially attenuate away the signal.
            EXPECT(rms_of_signal(a) > 0.10f);
        };

        if (channel_count == 1) {
            auto ladybird_output = ladybird_output_storage.span().slice(0, ladybird_result.output_frames_produced);
            i64 unused_shift = 0;
            check_channel(ladybird_output, 0, unused_shift);
        } else {
            auto ladybird_output0 = ladybird_output_l.span().slice(0, ladybird_result.output_frames_produced);
            auto ladybird_output1 = ladybird_output_r.span().slice(0, ladybird_result.output_frames_produced);
            i64 shift0 = 0;
            i64 shift1 = 0;
            check_channel(ladybird_output0, 0, shift0);
            check_channel(ladybird_output1, 1, shift1);
        }

        double const avg_ladybird_us = static_cast<double>(ladybird_us) / static_cast<double>(iterations);
        double const avg_ffmpeg_us = static_cast<double>(ffmpeg_us) / static_cast<double>(iterations);

        // Normalize by output scalar samples (frames * channels) to make it easier to compare the true inner-loop
        // throughput across different ratios/rates.
        double const ladybird_ns_per_output_sample = ladybird_result.output_frames_produced > 0
            ? (avg_ladybird_us * 1000.0) / static_cast<double>(ladybird_result.output_frames_produced * channel_count)
            : 0.0;
        double const ffmpeg_ns_per_output_sample = ffmpeg_produced > 0
            ? (avg_ffmpeg_us * 1000.0) / static_cast<double>(ffmpeg_produced * channel_count)
            : 0.0;

        outln("SRC {}->{} (ch={}, {}s, n={}): Ladybird {:.0f} us, FFmpeg {:.0f} us, ratio {:.2f}x, Ladybird {:.2f} ns/sample, FFmpeg {:.2f} ns/sample",
            pair.in_rate, pair.out_rate, channel_count, duration_seconds, iterations,
            avg_ladybird_us, avg_ffmpeg_us,
            avg_ffmpeg_us > 0.0 ? (avg_ladybird_us / avg_ffmpeg_us) : 0.0,
            ladybird_ns_per_output_sample, ffmpeg_ns_per_output_sample);

        swr_free(&ffmpeg_resampler.ctx);
    };

    for (auto pair : pairs)
        run_case(pair, 1);
    for (auto pair : stereo_pairs)
        run_case(pair, 2);

    outln("SRC totals ({} cases, {}s each, n={}): Ladybird {} us, FFmpeg {} us, ratio {:.2f}x", pairs.size() + stereo_pairs.size(), duration_seconds, iterations, total_ladybird_us, total_ffmpeg_us,
        total_ffmpeg_us > 0 ? static_cast<double>(total_ladybird_us) / static_cast<double>(total_ffmpeg_us) : 0.0);
}

}
