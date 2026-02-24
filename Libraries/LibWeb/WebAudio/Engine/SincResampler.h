/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Math.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibWeb/Export.h>

namespace Web::WebAudio::Render {

// Sample rate conversion via windowed-sinc interpolation and polyphase FIR.
// Described in https://en.wikipedia.org/wiki/Sample-rate_conversion &
// https://en.wikipedia.org/wiki/Sinc_interpolation.

// All usage of WEB_API here is for unit testing

// There are two ways to use this API:
// 1. For random-access sources (e.g. AudioBufferSource-style), use the
//    sinc_resampler_interpolate_at() function to fetch individual samples at arbitrary
//    input frame positions.
// 2. For streaming sources, use the SampleRateConverter class to maintain state.
//    Initialize it with sample_rate_converter_init(), then call
//    sample_rate_converter_process() to resample input blocks into output blocks.

// Precomputed polyphase coefficient table for interpolation.
struct SincResamplerKernel {
    static constexpr size_t phase_count = 256;
    static constexpr size_t tap_count = 32;

    Vector<f32> coefficients;
    f64 lowpass_scale { -1.0 };
};

// ============================= Random-access API =============================

// [control thread] Ensure the kernel table is built for input_frames_per_output_frame.
// May allocate/reallocate kernel.coefficients.
void prepare_sinc_resampler_kernel(SincResamplerKernel& kernel, f64 input_frames_per_output_frame);

// [render thread] Fetch the tap window for a phase. Allocation-free.
ALWAYS_INLINE ReadonlySpan<f32> sinc_resampler_phase_taps(SincResamplerKernel const& kernel, size_t phase_index)
{
    if (phase_index >= SincResamplerKernel::phase_count)
        phase_index = SincResamplerKernel::phase_count - 1;
    return kernel.coefficients.span().slice(phase_index * SincResamplerKernel::tap_count, SincResamplerKernel::tap_count);
}

// [render thread] For random-access sources (AudioBufferSource).
template<typename SampleAt>
requires(AK::Concepts::CallableAs<RemoveCVReference<SampleAt>, f32, size_t, i64>)
ALWAYS_INLINE f32 sinc_resampler_interpolate_at(SincResamplerKernel const& kernel, f64 position_in_input_frames, size_t channel, SampleAt&& sample_at)
{
    size_t const half = SincResamplerKernel::tap_count / 2;

    f64 const base_index_d = AK::floor(position_in_input_frames);
    f64 const frac = position_in_input_frames - base_index_d;

    size_t phase_index = static_cast<size_t>(frac * static_cast<f64>(SincResamplerKernel::phase_count));
    if (phase_index >= SincResamplerKernel::phase_count)
        phase_index = SincResamplerKernel::phase_count - 1;

    auto const coefficients = sinc_resampler_phase_taps(kernel, phase_index);
    i64 const base_index = static_cast<i64>(base_index_d);

    f32 sum = 0.0f;
    for (size_t tap = 0; tap < SincResamplerKernel::tap_count; ++tap) {
        i64 const k = static_cast<i64>(tap) - static_cast<i64>(half - 1);
        i64 const sample_index = base_index + k;
        sum += coefficients[tap] * sample_at(channel, sample_index);
    }
    return sum;
}

class SampleRateConverter;

// ================================ Streaming API ==============================

// Initializes state. If you change channels or sample ratio, you need to call this. May allocate.
WEB_API void sample_rate_converter_init(SampleRateConverter&, size_t channel_count, f64 input_frames_per_output_frame, size_t ring_size = 4096);

// Resets the resampler state. Allocation-free.
WEB_API void sample_rate_converter_reset(SampleRateConverter&);

// Updates the resampling ratio (input frames per output frame) without resetting state.
// This is intended for small continuous adjustments (e.g. drift correction).
WEB_API void sample_rate_converter_set_ratio(SampleRateConverter&, f64 input_frames_per_output_frame);

// Resample a block. Input/output channel counts must match state.channel_count.
// Input channel frame counts must be uniform, and output channel frame counts
// must be uniform. If flush is true, missing future input samples are treated as
// silence. This lets the resampler drain its internal state at end-of-stream.
struct ResampleResult {
    size_t input_frames_consumed { 0 };
    size_t output_frames_produced { 0 };
};
WEB_API ResampleResult sample_rate_converter_process(SampleRateConverter&, Span<ReadonlySpan<f32>> input_channels, Span<Span<f32>> output_channels, bool flush = false);

// Opaque parameter block for the sample_rate_converter_* functions above.
// Initialize via sample_rate_converter_init() before first use;
class SampleRateConverter {
public:
    SampleRateConverter() = default;
    SampleRateConverter(SampleRateConverter&&) = default;
    SampleRateConverter& operator=(SampleRateConverter&&) = default;
    SampleRateConverter(SampleRateConverter const&) = delete;
    SampleRateConverter& operator=(SampleRateConverter const&) = delete;
    ~SampleRateConverter() = default;

private:
    friend void sample_rate_converter_init(SampleRateConverter&, size_t channel_count, f64 input_frames_per_output_frame, size_t ring_size);
    friend void sample_rate_converter_reset(SampleRateConverter&);
    friend void sample_rate_converter_set_ratio(SampleRateConverter&, f64 input_frames_per_output_frame);
    friend ResampleResult sample_rate_converter_process(SampleRateConverter&, Span<ReadonlySpan<f32>> input_channels, Span<Span<f32>> output_channels, bool flush);

    f32* ring_base(size_t channel);
    f32 const* ring_base(size_t channel) const;
    f32 sample_at(size_t channel, i64 absolute_index) const;
    void write_input_frame(Span<ReadonlySpan<f32>> input_channels, size_t input_index);

    SincResamplerKernel table;
    f64 ratio { 1.0 };

    // Ring buffer storage is a flat array with per-channel segments.
    // Each channel segment has size ring_stride = ring_size + tap_count - 1, where the last
    // (tap_count - 1) samples mirror the beginning of the ring. This ensures any tap window
    // starting at a ring index is always contiguous in memory.
    Vector<f32> ring;
    size_t ring_size { 0 };
    size_t ring_stride { 0 };
    size_t channel_count { 0 };
    size_t write_index { 0 };
    size_t total_frames_written { 0 };

    f64 next_output_time_in_input_frames { 0.0 };
};

}
