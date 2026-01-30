/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/SIMDExtras.h>
#include <AK/StdLibExtras.h>
#include <LibWeb/WebAudio/Engine/SincResampler.h>

namespace Web::WebAudio::Render {

static f32 sinc_pi(f32 x)
{
    if (x == 0.0f)
        return 1.0f;
    f32 const pi_times_x = AK::Pi<f32> * x;
    return AK::sin(pi_times_x) / pi_times_x;
}

static f32 blackman_window(f64 i, f64 n_minus_1)
{
    // https://webaudio.github.io/web-audio-api/#blackman-window

    if (n_minus_1 == 0)
        return 1.0f;

    // Treat the window as having finite support. When we phase-shift by a fractional amount,
    // some taps may fall slightly outside [0, N-1]. Clamp them to zero.
    if (i < 0.0 || i > n_minus_1)
        return 0.0f;

    constexpr f32 a = 0.16f;
    constexpr f32 a0 = 0.5f * (1.0f - a);
    constexpr f32 a1 = 0.5f;
    constexpr f32 a2 = 0.5f * a;

    f32 const ratio = static_cast<f32>(i / n_minus_1);
    f32 const angle = 2.0f * AK::Pi<f32> * ratio;
    return a0 - (a1 * AK::cos(angle)) + (a2 * AK::cos(2.0f * angle));
}

void prepare_sinc_resampler_kernel(SincResamplerKernel& kernel, f64 input_frames_per_output_frame)
{
    // For downsampling, reduce the low-pass cutoff to avoid aliasing.
    // The ratio is expressed in input frames per output frame.
    f64 lowpass_scale = 1.0;
    if (__builtin_isfinite(input_frames_per_output_frame) && !__builtin_isnan(input_frames_per_output_frame) && input_frames_per_output_frame > 1.0)
        lowpass_scale = 1.0 / input_frames_per_output_frame;

    // Allocate once, then allow regeneration without resizing (render-thread friendly).
    size_t const expected_size = SincResamplerKernel::phase_count * SincResamplerKernel::tap_count;
    if (kernel.coefficients.size() != expected_size)
        kernel.coefficients.resize(expected_size);

    // Avoid regenerating if we're already configured for this low-pass scale.
    if (kernel.lowpass_scale >= 0.0 && AK::fabs(kernel.lowpass_scale - lowpass_scale) < 1e-15)
        return;

    kernel.lowpass_scale = lowpass_scale;

    size_t const half = SincResamplerKernel::tap_count / 2;
    f64 const n_minus_1 = static_cast<f64>(SincResamplerKernel::tap_count - 1);

    for (size_t phase = 0; phase < SincResamplerKernel::phase_count; ++phase) {
        f32 const frac = static_cast<f32>(phase) / static_cast<f32>(SincResamplerKernel::phase_count);

        f64 sum = 0.0;
        for (size_t tap = 0; tap < SincResamplerKernel::tap_count; ++tap) {
            // Tap index mapped to k in [-(half-1), +half]
            f32 const k = static_cast<f32>(static_cast<i32>(tap) - static_cast<i32>(half - 1));
            f32 const x = (k - frac) * static_cast<f32>(lowpass_scale);

            // Shift the window by the same sub-sample offset used for the shifted sinc.
            // This keeps the kernel phase-aligned at fractional positions.
            f32 const w = blackman_window(static_cast<f64>(tap) - static_cast<f64>(frac), n_minus_1);
            f32 const c = sinc_pi(x) * w;

            kernel.coefficients[(phase * SincResamplerKernel::tap_count) + tap] = c;
            sum += static_cast<f64>(c);
        }

        // Normalize to unity DC gain.
        if (sum != 0.0 && !__builtin_isnan(sum) && __builtin_isfinite(sum)) {
            f64 const inv_sum = 1.0 / sum;
            for (size_t tap = 0; tap < SincResamplerKernel::tap_count; ++tap)
                kernel.coefficients[(phase * SincResamplerKernel::tap_count) + tap] = static_cast<f32>(static_cast<f64>(kernel.coefficients[(phase * SincResamplerKernel::tap_count) + tap]) * inv_sum);

            // Reduce residual normalization error by nudging one tap so the coefficient sum is as close
            // to 1.0 as possible when accumulated in f64.
            f64 normalized_sum = 0.0;
            for (size_t tap = 0; tap < SincResamplerKernel::tap_count; ++tap)
                normalized_sum += static_cast<f64>(kernel.coefficients[(phase * SincResamplerKernel::tap_count) + tap]);

            f64 const correction = 1.0 - normalized_sum;
            if (!__builtin_isnan(correction) && __builtin_isfinite(correction)) {
                // Apply the correction to the tap with the largest magnitude.
                size_t best_tap = 0;
                f32 best_abs = 0.0f;
                for (size_t tap = 0; tap < SincResamplerKernel::tap_count; ++tap) {
                    f32 const abs_value = AK::fabs(kernel.coefficients[(phase * SincResamplerKernel::tap_count) + tap]);
                    if (abs_value > best_abs) {
                        best_abs = abs_value;
                        best_tap = tap;
                    }
                }
                kernel.coefficients[(phase * SincResamplerKernel::tap_count) + best_tap] += static_cast<f32>(correction);
            }
        }
    }
}

static ALWAYS_INLINE f32 dot_product_f32_simd(ReadonlySpan<f32> a, ReadonlySpan<f32> b)
{
    VERIFY(a.size() == b.size());

    using AK::SIMD::f32x4;
    size_t i = 0;
    f32x4 acc { 0.0f, 0.0f, 0.0f, 0.0f };
    for (; i + 4 <= a.size(); i += 4) {
        auto const av = AK::SIMD::load_unaligned<f32x4>(a.data() + i);
        auto const bv = AK::SIMD::load_unaligned<f32x4>(b.data() + i);
        acc += av * bv;
    }
    f32 sum = acc[0] + acc[1] + acc[2] + acc[3];
    for (; i < a.size(); ++i)
        sum += a[i] * b[i];
    return sum;
}

// Notes:
// - The resampling ratio is input frames per output frame.
// - The filter is symmetric so producing output may require input lookahead.
// - Keep a shared ratio across channels because we process all channels in lockstep.

// Initializes state. If you change channels or sample ratio, you need to call this. May allocate.
void sample_rate_converter_init(SampleRateConverter& state, size_t channel_count, f64 input_frames_per_output_frame, size_t ring_size)
{
    state.ratio = input_frames_per_output_frame;
    prepare_sinc_resampler_kernel(state.table, state.ratio);
    VERIFY(ring_size >= SincResamplerKernel::tap_count + 2);
    state.ring_size = ring_size;
    state.ring_stride = state.ring_size + (SincResamplerKernel::tap_count - 1);
    state.channel_count = max<size_t>(1, channel_count);
    state.ring.resize(state.channel_count * state.ring_stride);
    sample_rate_converter_reset(state);
}

// Resets the resampler state. Allocation-free.
void sample_rate_converter_reset(SampleRateConverter& state)
{
    state.write_index = 0;
    state.total_frames_written = 0;
    state.next_output_time_in_input_frames = 0.0;
    state.ring.fill(0.0f);
}

void sample_rate_converter_set_ratio(SampleRateConverter& state, f64 input_frames_per_output_frame)
{
    state.ratio = input_frames_per_output_frame;
}

// Resample a block. Input/output channel counts must match state.channel_count.
// Input channel frame counts must be uniform, and output channel frame counts must be uniform.
// If flush is true, missing future input samples are treated as silence. This lets the
// resampler drain its internal state at end-of-stream.
ResampleResult sample_rate_converter_process(SampleRateConverter& state, Span<ReadonlySpan<f32>> input_channels, Span<Span<f32>> output_channels, bool flush)
{
    VERIFY(input_channels.size() == state.channel_count);
    VERIFY(output_channels.size() == state.channel_count);

    if (!__builtin_isfinite(state.ratio) || __builtin_isnan(state.ratio) || state.ratio <= 0.0)
        return {};

    if (input_channels.is_empty() || output_channels.is_empty())
        return {};

    size_t const input_frames = input_channels[0].size();
    for (size_t ch = 1; ch < state.channel_count; ++ch)
        VERIFY(input_channels[ch].size() == input_frames);

    size_t const output_frames = output_channels[0].size();
    for (size_t ch = 1; ch < state.channel_count; ++ch)
        VERIFY(output_channels[ch].size() == output_frames);

    size_t consumed = 0;
    size_t produced = 0;

    auto const& table = state.table;
    size_t const half = SincResamplerKernel::tap_count / 2;

    auto sample_at = [&](size_t channel, i64 sample_index) -> f32 {
        return state.sample_at(channel, sample_index);
    };

    while (produced < output_frames) {
        // Ensure we have enough input lookahead to produce the next output sample.
        i64 const base_index = static_cast<i64>(AK::floor(state.next_output_time_in_input_frames));
        i64 const required_max_index = base_index + static_cast<i64>(half);

        while (static_cast<i64>(state.total_frames_written) <= required_max_index && consumed < input_frames)
            state.write_input_frame(input_channels, consumed++);

        if (static_cast<i64>(state.total_frames_written) <= required_max_index && !flush)
            break;

        // Fast-path: when the entire tap window is fully available in the ring buffer, avoid
        // per-tap bounds checks and modulo operations.
        f64 const position_in_input_frames = state.next_output_time_in_input_frames;
        f64 const base_index_d = AK::floor(position_in_input_frames);
        f64 const frac = position_in_input_frames - base_index_d;

        size_t phase_index = static_cast<size_t>(frac * static_cast<f64>(SincResamplerKernel::phase_count));
        if (phase_index >= SincResamplerKernel::phase_count)
            phase_index = SincResamplerKernel::phase_count - 1;

        auto const coefficients = sinc_resampler_phase_taps(table, phase_index);

        i64 const start_index = base_index - static_cast<i64>(half - 1);
        i64 const end_index = base_index + static_cast<i64>(half);

        bool const window_fully_available = start_index >= 0
            && end_index < static_cast<i64>(state.total_frames_written)
            && (state.total_frames_written <= state.ring_size
                || static_cast<u64>(start_index) >= (state.total_frames_written - state.ring_size));

        if (window_fully_available) {
            size_t const start_ring_index = static_cast<size_t>(start_index) % state.ring_size;
            // Mono/stereo specialization helps the compiler optimize better than a runtime channel loop.
            if (state.channel_count == 1) {
                auto sample_span = ReadonlySpan<f32> { state.ring_base(0) + start_ring_index, SincResamplerKernel::tap_count };
                output_channels[0][produced] = dot_product_f32_simd(coefficients, sample_span);
            } else if (state.channel_count == 2) {
                auto sample_span_l = ReadonlySpan<f32> { state.ring_base(0) + start_ring_index, SincResamplerKernel::tap_count };
                auto sample_span_r = ReadonlySpan<f32> { state.ring_base(1) + start_ring_index, SincResamplerKernel::tap_count };
                output_channels[0][produced] = dot_product_f32_simd(coefficients, sample_span_l);
                output_channels[1][produced] = dot_product_f32_simd(coefficients, sample_span_r);
            } else {
                for (size_t ch = 0; ch < state.channel_count; ++ch) {
                    auto sample_span = ReadonlySpan<f32> { state.ring_base(ch) + start_ring_index, SincResamplerKernel::tap_count };
                    output_channels[ch][produced] = dot_product_f32_simd(coefficients, sample_span);
                }
            }
        } else {
            // Mono/stereo specialization helps the compiler optimize better than a runtime channel loop.
            if (state.channel_count == 1) {
                output_channels[0][produced] = sinc_resampler_interpolate_at(table, position_in_input_frames, 0, sample_at);
            } else if (state.channel_count == 2) {
                output_channels[0][produced] = sinc_resampler_interpolate_at(table, position_in_input_frames, 0, sample_at);
                output_channels[1][produced] = sinc_resampler_interpolate_at(table, position_in_input_frames, 1, sample_at);
            } else {
                for (size_t ch = 0; ch < state.channel_count; ++ch)
                    output_channels[ch][produced] = sinc_resampler_interpolate_at(table, position_in_input_frames, ch, sample_at);
            }
        }

        ++produced;
        state.next_output_time_in_input_frames += state.ratio;
    }

    return { consumed, produced };
}

ALWAYS_INLINE f32* SampleRateConverter::ring_base(size_t channel)
{
    return ring.data() + (channel * ring_stride);
}

ALWAYS_INLINE f32 const* SampleRateConverter::ring_base(size_t channel) const
{
    return ring.data() + (channel * ring_stride);
}

ALWAYS_INLINE f32 SampleRateConverter::sample_at(size_t channel, i64 absolute_index) const
{
    // Pre-roll is silence.
    if (absolute_index < 0)
        return 0.0f;

    // Future samples are silence.
    if (static_cast<u64>(absolute_index) >= total_frames_written)
        return 0.0f;

    // The ring buffer stores the most recent ring_size samples.
    // If the requested index is too old, it has been overwritten.
    if (total_frames_written > ring_size && static_cast<u64>(absolute_index) < (total_frames_written - ring_size))
        return 0.0f;

    size_t const index = static_cast<size_t>(absolute_index) % ring_size;
    return ring_base(channel)[index];
}

ALWAYS_INLINE void SampleRateConverter::write_input_frame(Span<ReadonlySpan<f32>> input_channels, size_t input_index)
{
    // Fast paths for the common mono/stereo cases.
    // Specializing outside the runtime channel loop tends to generate tighter code (less loop/control overhead,
    // better aliasing assumptions, better scheduling) in this hot path.
    if (channel_count == 1) {
        auto* channel_ring = ring_base(0);
        f32 const sample = input_channels[0][input_index];
        channel_ring[write_index] = sample;
        if (write_index < (SincResamplerKernel::tap_count - 1))
            channel_ring[write_index + ring_size] = sample;
        write_index = (write_index + 1) % ring_size;
        ++total_frames_written;
        return;
    }

    if (channel_count == 2) {
        auto* ring_l = ring_base(0);
        auto* ring_r = ring_base(1);

        f32 const sample_l = input_channels[0][input_index];
        f32 const sample_r = input_channels[1][input_index];

        ring_l[write_index] = sample_l;
        ring_r[write_index] = sample_r;

        if (write_index < (SincResamplerKernel::tap_count - 1)) {
            size_t const mirror_index = write_index + ring_size;
            ring_l[mirror_index] = sample_l;
            ring_r[mirror_index] = sample_r;
        }

        write_index = (write_index + 1) % ring_size;
        ++total_frames_written;
        return;
    }

    for (size_t ch = 0; ch < channel_count; ++ch) {
        auto* channel_ring = ring_base(ch);
        f32 const sample = input_channels[ch][input_index];
        channel_ring[write_index] = sample;
        // Mirror only the prefix [0, tap_count-2] at the end of the ring.
        // This ensures any tap window is contiguous without requiring a full extra ring_size.
        if (write_index < (SincResamplerKernel::tap_count - 1))
            channel_ring[write_index + ring_size] = sample;
    }
    write_index = (write_index + 1) % ring_size;
    ++total_frames_written;
}

}
