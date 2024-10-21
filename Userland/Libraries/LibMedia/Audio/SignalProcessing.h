/*
 * Copyright (c) 2024, Noah Bright <noah.bright.1@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Complex.h>
#include <AK/Vector.h>

namespace Audio {

template<AK::Concepts::Arithmetic T>
Vector<Complex<T>> biquad_filter_frequency_response(Vector<T> const& frequencies, Array<T, 6> const& coefficients)
{
    // frequencies is expected to be frequencies in Hz, not angular frequencies in rad/sec
    // coefficients should be a fixed sized array of 6 containing [b0, b1, b2, a0, a1, a2]
    //
    // the transfer function of a biquadratic filter is
    //      H(z) = ( b0/a0 + b1/a0 * z^-1 + b2/a0 * z^-2 ) / ( 1 + a1/a0 * z^-1 + a2/a0 * z^-2)
    //
    // as written, the numerator and denominator above both have 3 floating point multiplications
    // rewriting the numerator, that can be reduced to 2:
    // b0/a0 + (b1/a0 + b2/a0 * z) * z^-1
    //
    // the frequency response of a filter at frequency omega is its transfer function evaluated
    // at z = e^{i omega}, with angular frequency omega = 2*pi*f
    //
    // the magnitude(phase) response of a filter is the magnitude(phase) of its frequency response

    T B0 = coefficients[0] / coefficients[3];
    T B1 = coefficients[1] / coefficients[3];
    T B2 = coefficients[2] / coefficients[3];
    T A1 = coefficients[4] / coefficients[3];
    T A2 = coefficients[5] / coefficients[3];

    auto transfer_function = [&](Complex<T> z) {
        auto z_inv = 1 / z;
        auto numerator = B0 + (B1 + B2 * z_inv) * z_inv;
        auto denominator = 1.0 + (A1 + A2 * z_inv) * z_inv;
        return numerator / denominator;
    };

    Vector<Complex<T>> frequency_response;
    frequency_response.ensure_capacity(frequencies.size());

    for (size_t k = 0; k < frequency_response.size(); k++) {
        auto i = Complex<T>(0, 1);
        auto arg = cexp(2 * AK::Pi<T> * i * frequencies[k]);
        frequency_response[k] = transfer_function(arg);
    }

    return frequency_response;
}

template<AK::Concepts::Arithmetic T>
Array<T, 6> biquad_filter_lowpass_coefficients(T omega_0, T alpha_Q_dB)
{
    auto b0 = 0.5 * (1 - cos(omega_0));
    auto b1 = 1 - cos(omega_0);
    auto b2 = b0;
    auto a0 = 1 + alpha_Q_dB;
    auto a1 = -2 * cos(omega_0);
    auto a2 = 1 - alpha_Q_dB;
    return { b0, b1, b2, a0, a1, a2 };
}

}
