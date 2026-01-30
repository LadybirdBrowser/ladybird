/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/SIMDExtras.h>
#include <AK/StdLibExtras.h>
#include <LibWeb/WebAudio/Engine/FrequencyAnalysis.h>

namespace Web::WebAudio::Render {

// https://webaudio.github.io/web-audio-api/#blackman-window
static Span<f32> ensure_blackman_window(FrequencyAnalysisScratch& scratch, size_t fft_size)
{
    if (scratch.blackman_window.size() < fft_size)
        scratch.blackman_window.resize(fft_size);

    if (scratch.blackman_window_size == fft_size)
        return scratch.blackman_window.span().slice(0, fft_size);

    auto window = scratch.blackman_window.span().slice(0, fft_size);
    if (fft_size == 0) {
        scratch.blackman_window_size = 0;
        return window;
    }

    f32 const a = 0.16f;
    f32 const a0 = 0.5f * (1.0f - a);
    f32 const a1 = 0.5f;
    f32 const a2 = a * 0.5f;
    f32 const N = static_cast<f32>(fft_size);

    for (size_t i = 0; i < fft_size; ++i) {
        f32 const n = static_cast<f32>(i);
        window[i] = a0
            - (a1 * AK::cos(2.0f * AK::Pi<f32> * n / N))
            + (a2 * AK::cos(4.0f * AK::Pi<f32> * n / N));
    }

    scratch.blackman_window_size = fft_size;
    return window;
}

template<typename T>
static ALWAYS_INLINE void complex_multiply(T a_real, T a_imag, T b_real, T b_imag, T& out_real, T& out_imag)
{
    if constexpr (IsSame<T, f32>) {
        AK::SIMD::f32x4 a { a_real, a_imag, a_real, a_imag };
        AK::SIMD::f32x4 b { b_real, b_imag, b_imag, b_real };
        AK::SIMD::f32x4 prod = a * b;
        out_real = prod[0] - prod[1];
        out_imag = prod[2] + prod[3];
    } else if constexpr (IsSame<T, f64>) {
        AK::SIMD::f64x2 a { a_real, a_imag };
        AK::SIMD::f64x2 b { b_real, b_imag };
        AK::SIMD::f64x2 prod = a * b;
        AK::SIMD::f64x2 b_swapped { b_imag, b_real };
        AK::SIMD::f64x2 prod_swapped = a * b_swapped;
        out_real = prod[0] - prod[1];
        out_imag = prod_swapped[0] + prod_swapped[1];
    } else {
        out_real = (a_real * b_real) - (a_imag * b_imag);
        out_imag = (a_real * b_imag) + (a_imag * b_real);
    }
}

template<typename T>
static void fft_in_place(Span<T> real, Span<T> imaginary, FFTDirection direction = FFTDirection::Forward)
{
    VERIFY(real.size() == imaginary.size());
    size_t const N = real.size();
    if (N == 0)
        return;

    VERIFY(is_power_of_two(N));

    for (size_t i = 1, j = 0; i < N; ++i) {
        size_t bit = N >> 1;
        while (j & bit) {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;

        if (i < j) {
            AK::swap(real[i], real[j]);
            AK::swap(imaginary[i], imaginary[j]);
        }
    }

    T const sign = direction == FFTDirection::Forward ? static_cast<T>(-1.0) : static_cast<T>(1.0);

    for (size_t n = N, s = 1; n > 1; n >>= 1, ++s) {
        size_t const m = 1u << s;
        size_t const half_m = m >> 1;

        T const angle = sign * static_cast<T>(2.0) * AK::Pi<T> / static_cast<T>(m);
        T wm_sin;
        T wm_cos;
        AK::sincos(angle, wm_sin, wm_cos);

        for (size_t k = 0; k < N; k += m) {
            T w_cos = static_cast<T>(1.0);
            T w_sin = static_cast<T>(0.0);

            for (size_t j = 0; j < half_m; ++j) {
                size_t const u_index = k + j;
                size_t const v_index = u_index + half_m;

                T t_real = static_cast<T>(0.0);
                T t_imag = static_cast<T>(0.0);
                complex_multiply(real[v_index], imaginary[v_index], w_cos, w_sin, t_real, t_imag);

                T const u_real = real[u_index];
                T const u_imag = imaginary[u_index];

                real[u_index] = u_real + t_real;
                imaginary[u_index] = u_imag + t_imag;

                real[v_index] = u_real - t_real;
                imaginary[v_index] = u_imag - t_imag;

                T const next_w_cos = (w_cos * wm_cos) - (w_sin * wm_sin);
                T const next_w_sin = (w_cos * wm_sin) + (w_sin * wm_cos);
                w_cos = next_w_cos;
                w_sin = next_w_sin;
            }
        }
    }

    if (direction == FFTDirection::Inverse) {
        T const inv_N = static_cast<T>(1.0) / static_cast<T>(N);
        for (size_t i = 0; i < N; ++i) {
            real[i] *= inv_N;
            imaginary[i] *= inv_N;
        }
    }
}

void apply_fft_in_place(Span<f64> real, Span<f64> imaginary, FFTDirection direction)
{
    fft_in_place(real, imaginary, direction);
}

// https://webaudio.github.io/web-audio-api/#smoothing-over-time
static void convert_fft_to_smoothed_db_in_place(ReadonlySpan<f32> real, ReadonlySpan<f32> imaginary, f32 smoothing_time_constant, Span<f32> previous_block, Span<f32> output_db)
{
    VERIFY(real.size() == imaginary.size());
    size_t const fft_size = real.size();
    VERIFY(is_power_of_two(fft_size));

    // Our FFT implementation uses an unnormalized forward transform, so normalize here.
    f32 const magnitude_scale = 1.0f / static_cast<f32>(fft_size);

    size_t const bin_count = fft_size / 2;
    VERIFY(previous_block.size() >= bin_count);
    VERIFY(output_db.size() >= bin_count);

    auto previous = previous_block.slice(0, bin_count);
    auto output = output_db.slice(0, bin_count);

    for (size_t i = 0; i < bin_count; ++i) {
        f32 const re = real[i];
        f32 const im = imaginary[i];
        f32 const magnitude = AK::sqrt((re * re) + (im * im)) * magnitude_scale;

        f32 const smoothed = (smoothing_time_constant * previous[i]) + ((1.0f - smoothing_time_constant) * magnitude);
        previous[i] = smoothed;

        // https://webaudio.github.io/web-audio-api/#conversion-to-db
        if (smoothed <= 0.0f || __builtin_isnan(smoothed))
            output[i] = -AK::Infinity<f32>;
        else
            output[i] = 20.0f * AK::log10(smoothed);
    }
}

// https://webaudio.github.io/web-audio-api/#fft-windowing-and-smoothing-over-time
void compute_frequency_data_db_in_place(ReadonlySpan<f32> time_domain_data, size_t fft_size, f32 smoothing_time_constant, Vector<f32>& previous_block, Vector<f32>& output_db, FrequencyAnalysisScratch& scratch)
{
    VERIFY(fft_size > 0);
    VERIFY(time_domain_data.size() == fft_size);
    VERIFY(is_power_of_two(fft_size));

    size_t const bin_count = fft_size / 2;
    VERIFY(previous_block.size() >= bin_count);
    VERIFY(output_db.size() >= bin_count);
    VERIFY(scratch.real.size() >= fft_size);
    VERIFY(scratch.imaginary.size() >= fft_size);

    auto real = scratch.real.span().slice(0, fft_size);
    auto imaginary = scratch.imaginary.span().slice(0, fft_size);

    // 2. Apply a Blackman window to the time domain input data.
    auto const window = ensure_blackman_window(scratch, fft_size);
    for (size_t i = 0; i < fft_size; ++i) {
        real[i] = time_domain_data[i] * window[i];
        imaginary[i] = 0.0f;
    }

    // 3. Apply a Fourier transform to the windowed time domain input data to get real and imaginary frequency data.
    fft_in_place(real, imaginary);

    // 4. Smooth over time the frequency domain data.
    // 5. Convert to dB.
    convert_fft_to_smoothed_db_in_place(real, imaginary, smoothing_time_constant, previous_block.span(), output_db.span());
}

}
