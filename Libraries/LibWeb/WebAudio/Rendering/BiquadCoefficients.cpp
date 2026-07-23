/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/WebAudio/Rendering/BiquadCoefficients.h>

namespace Web::WebAudio::Rendering {

// https://webaudio.github.io/web-audio-api/#filters-characteristics
BiquadCoefficients compute_biquad_coefficients(Bindings::BiquadFilterType type, double normalized_frequency, double q, double gain_db)
{
    auto omega = AK::Pi<double> * normalized_frequency;
    auto sin_omega = AK::sin(omega);
    auto cos_omega = AK::cos(omega);
    auto a = AK::pow(10., gain_db / 40.);

    double b0 = 1;
    double b1 = 0;
    double b2 = 0;
    double a0 = 1;
    double a1 = 0;
    double a2 = 0;

    switch (type) {
    case Bindings::BiquadFilterType::Lowpass: {
        // NB: For lowpass and highpass filters, Q is interpreted as being in dB.
        auto alpha_q_db = sin_omega / (2 * AK::pow(10., q / 20.));
        b0 = (1 - cos_omega) / 2;
        b1 = 1 - cos_omega;
        b2 = b0;
        a0 = 1 + alpha_q_db;
        a1 = -2 * cos_omega;
        a2 = 1 - alpha_q_db;
        break;
    }
    case Bindings::BiquadFilterType::Highpass: {
        auto alpha_q_db = sin_omega / (2 * AK::pow(10., q / 20.));
        b0 = (1 + cos_omega) / 2;
        b1 = -(1 + cos_omega);
        b2 = b0;
        a0 = 1 + alpha_q_db;
        a1 = -2 * cos_omega;
        a2 = 1 - alpha_q_db;
        break;
    }
    case Bindings::BiquadFilterType::Bandpass: {
        auto alpha_q = q > 0 ? sin_omega / (2 * q) : 1;
        b0 = alpha_q;
        b1 = 0;
        b2 = -alpha_q;
        a0 = 1 + alpha_q;
        a1 = -2 * cos_omega;
        a2 = 1 - alpha_q;
        break;
    }
    case Bindings::BiquadFilterType::Notch: {
        auto alpha_q = q > 0 ? sin_omega / (2 * q) : 1;
        b0 = 1;
        b1 = -2 * cos_omega;
        b2 = 1;
        a0 = 1 + alpha_q;
        a1 = b1;
        a2 = 1 - alpha_q;
        break;
    }
    case Bindings::BiquadFilterType::Allpass: {
        auto alpha_q = q > 0 ? sin_omega / (2 * q) : 1;
        b0 = 1 - alpha_q;
        b1 = -2 * cos_omega;
        b2 = 1 + alpha_q;
        a0 = 1 + alpha_q;
        a1 = b1;
        a2 = 1 - alpha_q;
        break;
    }
    case Bindings::BiquadFilterType::Peaking: {
        auto alpha_q = q > 0 ? sin_omega / (2 * q) : 1;
        b0 = 1 + alpha_q * a;
        b1 = -2 * cos_omega;
        b2 = 1 - alpha_q * a;
        a0 = 1 + alpha_q / a;
        a1 = b1;
        a2 = 1 - alpha_q / a;
        break;
    }
    case Bindings::BiquadFilterType::Lowshelf: {
        // NB: For the shelf filters, α_S uses a shelf slope S of 1.
        auto alpha_s = sin_omega / 2 * AK::sqrt(2.);
        auto two_sqrt_a_alpha_s = 2 * AK::sqrt(a) * alpha_s;
        b0 = a * ((a + 1) - (a - 1) * cos_omega + two_sqrt_a_alpha_s);
        b1 = 2 * a * ((a - 1) - (a + 1) * cos_omega);
        b2 = a * ((a + 1) - (a - 1) * cos_omega - two_sqrt_a_alpha_s);
        a0 = (a + 1) + (a - 1) * cos_omega + two_sqrt_a_alpha_s;
        a1 = -2 * ((a - 1) + (a + 1) * cos_omega);
        a2 = (a + 1) + (a - 1) * cos_omega - two_sqrt_a_alpha_s;
        break;
    }
    case Bindings::BiquadFilterType::Highshelf: {
        auto alpha_s = sin_omega / 2 * AK::sqrt(2.);
        auto two_sqrt_a_alpha_s = 2 * AK::sqrt(a) * alpha_s;
        b0 = a * ((a + 1) + (a - 1) * cos_omega + two_sqrt_a_alpha_s);
        b1 = -2 * a * ((a - 1) + (a + 1) * cos_omega);
        b2 = a * ((a + 1) + (a - 1) * cos_omega - two_sqrt_a_alpha_s);
        a0 = (a + 1) - (a - 1) * cos_omega + two_sqrt_a_alpha_s;
        a1 = 2 * ((a - 1) - (a + 1) * cos_omega);
        a2 = (a + 1) - (a - 1) * cos_omega - two_sqrt_a_alpha_s;
        break;
    }
    }

    return BiquadCoefficients {
        .b0 = b0 / a0,
        .b1 = b1 / a0,
        .b2 = b2 / a0,
        .a1 = a1 / a0,
        .a2 = a2 / a0,
    };
}

// https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-getfrequencyresponse
void biquad_frequency_response(BiquadCoefficients const& coefficients, double normalized_frequency, float& magnitude, float& phase)
{
    // NB: The response is evaluated at z = e^(jω) with ω = π * normalized_frequency:
    //         H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2)
    auto omega = AK::Pi<double> * normalized_frequency;
    auto cos_omega = AK::cos(omega);
    auto sin_omega = AK::sin(omega);
    auto cos_2_omega = AK::cos(2 * omega);
    auto sin_2_omega = AK::sin(2 * omega);
    auto numerator_real = coefficients.b0 + coefficients.b1 * cos_omega + coefficients.b2 * cos_2_omega;
    auto numerator_imaginary = -(coefficients.b1 * sin_omega + coefficients.b2 * sin_2_omega);
    auto denominator_real = 1 + coefficients.a1 * cos_omega + coefficients.a2 * cos_2_omega;
    auto denominator_imaginary = -(coefficients.a1 * sin_omega + coefficients.a2 * sin_2_omega);
    auto denominator_magnitude_squared = denominator_real * denominator_real + denominator_imaginary * denominator_imaginary;
    auto response_real = (numerator_real * denominator_real + numerator_imaginary * denominator_imaginary) / denominator_magnitude_squared;
    auto response_imaginary = (numerator_imaginary * denominator_real - numerator_real * denominator_imaginary) / denominator_magnitude_squared;
    magnitude = static_cast<float>(AK::sqrt(response_real * response_real + response_imaginary * response_imaginary));
    phase = static_cast<float>(AK::atan2(response_imaginary, response_real));
}

}
