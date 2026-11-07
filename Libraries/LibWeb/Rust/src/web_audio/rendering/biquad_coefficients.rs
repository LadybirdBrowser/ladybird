/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::math;

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum BiquadFilterType {
    Lowpass,
    Highpass,
    Bandpass,
    Lowshelf,
    Highshelf,
    Peaking,
    Notch,
    Allpass,
}

// Normalized biquad transfer function coefficients, with a0 divided out.
// https://webaudio.github.io/web-audio-api/#filters-characteristics
#[derive(Clone, Copy)]
#[repr(C)]
pub struct BiquadCoefficients {
    pub b0: f64,
    pub b1: f64,
    pub b2: f64,
    pub a1: f64,
    pub a2: f64,
}

// Computes filter coefficients from the BiquadFilterNode's computed parameter values. frequency is expected to already
// include the detune factor and be given as a fraction of the Nyquist frequency, in the range [0, 1].
// https://webaudio.github.io/web-audio-api/#filters-characteristics
#[allow(unused_assignments)]
pub fn compute_biquad_coefficients(
    filter_type: BiquadFilterType,
    normalized_frequency: f64,
    q: f64,
    gain_db: f64,
) -> BiquadCoefficients {
    let omega = std::f64::consts::PI * normalized_frequency;
    let sin_omega = math::sin(omega);
    let cos_omega = math::cos(omega);
    let a = math::pow(10., gain_db / 40.);

    let mut b0 = 1.0;
    let mut b1 = 0.0;
    let mut b2 = 0.0;
    let mut a0 = 1.0;
    let mut a1 = 0.0;
    let mut a2 = 0.0;

    match filter_type {
        BiquadFilterType::Lowpass => {
            // NB: For lowpass and highpass filters, Q is interpreted as being in dB.
            let alpha_q_db = sin_omega / (2. * math::pow(10., q / 20.));
            b0 = (1. - cos_omega) / 2.;
            b1 = 1. - cos_omega;
            b2 = b0;
            a0 = 1. + alpha_q_db;
            a1 = -2. * cos_omega;
            a2 = 1. - alpha_q_db;
        }
        BiquadFilterType::Highpass => {
            let alpha_q_db = sin_omega / (2. * math::pow(10., q / 20.));
            b0 = (1. + cos_omega) / 2.;
            b1 = -(1. + cos_omega);
            b2 = b0;
            a0 = 1. + alpha_q_db;
            a1 = -2. * cos_omega;
            a2 = 1. - alpha_q_db;
        }
        BiquadFilterType::Bandpass => {
            let alpha_q = if q > 0. { sin_omega / (2. * q) } else { 1. };
            b0 = alpha_q;
            b1 = 0.;
            b2 = -alpha_q;
            a0 = 1. + alpha_q;
            a1 = -2. * cos_omega;
            a2 = 1. - alpha_q;
        }
        BiquadFilterType::Notch => {
            let alpha_q = if q > 0. { sin_omega / (2. * q) } else { 1. };
            b0 = 1.;
            b1 = -2. * cos_omega;
            b2 = 1.;
            a0 = 1. + alpha_q;
            a1 = b1;
            a2 = 1. - alpha_q;
        }
        BiquadFilterType::Allpass => {
            let alpha_q = if q > 0. { sin_omega / (2. * q) } else { 1. };
            b0 = 1. - alpha_q;
            b1 = -2. * cos_omega;
            b2 = 1. + alpha_q;
            a0 = 1. + alpha_q;
            a1 = b1;
            a2 = 1. - alpha_q;
        }
        BiquadFilterType::Peaking => {
            let alpha_q = if q > 0. { sin_omega / (2. * q) } else { 1. };
            b0 = 1. + alpha_q * a;
            b1 = -2. * cos_omega;
            b2 = 1. - alpha_q * a;
            a0 = 1. + alpha_q / a;
            a1 = b1;
            a2 = 1. - alpha_q / a;
        }
        BiquadFilterType::Lowshelf => {
            // NB: For the shelf filters, α_S uses a shelf slope S of 1.
            let alpha_s = sin_omega / 2. * 2.0_f64.sqrt();
            let two_sqrt_a_alpha_s = 2. * a.sqrt() * alpha_s;
            b0 = a * ((a + 1.) - (a - 1.) * cos_omega + two_sqrt_a_alpha_s);
            b1 = 2. * a * ((a - 1.) - (a + 1.) * cos_omega);
            b2 = a * ((a + 1.) - (a - 1.) * cos_omega - two_sqrt_a_alpha_s);
            a0 = (a + 1.) + (a - 1.) * cos_omega + two_sqrt_a_alpha_s;
            a1 = -2. * ((a - 1.) + (a + 1.) * cos_omega);
            a2 = (a + 1.) + (a - 1.) * cos_omega - two_sqrt_a_alpha_s;
        }
        BiquadFilterType::Highshelf => {
            let alpha_s = sin_omega / 2. * 2.0_f64.sqrt();
            let two_sqrt_a_alpha_s = 2. * a.sqrt() * alpha_s;
            b0 = a * ((a + 1.) + (a - 1.) * cos_omega + two_sqrt_a_alpha_s);
            b1 = -2. * a * ((a - 1.) + (a + 1.) * cos_omega);
            b2 = a * ((a + 1.) + (a - 1.) * cos_omega - two_sqrt_a_alpha_s);
            a0 = (a + 1.) - (a - 1.) * cos_omega + two_sqrt_a_alpha_s;
            a1 = 2. * ((a - 1.) - (a + 1.) * cos_omega);
            a2 = (a + 1.) - (a - 1.) * cos_omega - two_sqrt_a_alpha_s;
        }
    }

    BiquadCoefficients {
        b0: b0 / a0,
        b1: b1 / a0,
        b2: b2 / a0,
        a1: a1 / a0,
        a2: a2 / a0,
    }
}

// Evaluates the transfer function's magnitude and phase at the given frequency, given as a fraction of the Nyquist
// frequency in the range [0, 1].
// https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-getfrequencyresponse
pub fn biquad_frequency_response(coefficients: &BiquadCoefficients, normalized_frequency: f64) -> (f32, f32) {
    // NB: The response is evaluated at z = e^(jω) with ω = π * normalized_frequency:
    //         H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2)
    let omega = std::f64::consts::PI * normalized_frequency;
    let cos_omega = math::cos(omega);
    let sin_omega = math::sin(omega);
    let cos_2_omega = math::cos(2. * omega);
    let sin_2_omega = math::sin(2. * omega);
    let numerator_real = coefficients.b0 + coefficients.b1 * cos_omega + coefficients.b2 * cos_2_omega;
    let numerator_imaginary = -(coefficients.b1 * sin_omega + coefficients.b2 * sin_2_omega);
    let denominator_real = 1. + coefficients.a1 * cos_omega + coefficients.a2 * cos_2_omega;
    let denominator_imaginary = -(coefficients.a1 * sin_omega + coefficients.a2 * sin_2_omega);
    let denominator_magnitude_squared =
        denominator_real * denominator_real + denominator_imaginary * denominator_imaginary;
    let response_real = (numerator_real * denominator_real + numerator_imaginary * denominator_imaginary)
        / denominator_magnitude_squared;
    let response_imaginary = (numerator_imaginary * denominator_real - numerator_real * denominator_imaginary)
        / denominator_magnitude_squared;
    let magnitude = (response_real * response_real + response_imaginary * response_imaginary).sqrt() as f32;
    let phase = math::atan2(response_imaginary, response_real) as f32;
    (magnitude, phase)
}
