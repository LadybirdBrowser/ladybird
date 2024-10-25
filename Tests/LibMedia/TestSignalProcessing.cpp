/*
 * Copyright (c) 2024, Noah Bright <noah.bright.1@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Complex.h>
#include <LibTest/TestCase.h>

#include <LibMedia/Audio/SignalProcessing.h>

TEST_CASE(test_biquad_filter_frequency_response)
{
    size_t n_frequencies = 4096;
    Vector<f64> frequencies;
    frequencies.resize(n_frequencies);

    // roughly the range of human hearing in Hz
    auto delta_f = (20000 - 20) / n_frequencies;
    for (size_t i = 0; i < n_frequencies; i++)
        frequencies[i] = 20.0 + (f64)i * (f64)delta_f;

    // default coefficients from https://webaudio.github.io/web-audio-api/#BiquadFilterOptions
    f64 Q = 1.0;
    f64 frequency = 350;
    f64 sample_rate = 44100;

    auto omega_0 = 2 * AK::Pi<f64> * frequency / sample_rate;
    auto alpha_Q_dB = sin(omega_0) / (2 * pow(10, Q / 20));

    auto lowpass_coeffs = Audio::biquad_filter_lowpass_coefficients(omega_0, alpha_Q_dB);

    auto frequency_response = Audio::biquad_filter_frequency_response(frequencies, lowpass_coeffs);
    for (auto response : frequency_response) {
        auto phase_response = response.phase();
        VERIFY(phase_response != AK::NaN<f64>);
        VERIFY(phase_response != AK::Infinity<f64>);

        auto magnitude_response = response.magnitude();
        VERIFY(magnitude_response != AK::NaN<f64>);
        VERIFY(magnitude_response != AK::Infinity<f64>);
    }
}
