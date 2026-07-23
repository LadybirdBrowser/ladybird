/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/BiquadFilterNode.h>
#include <LibWeb/Export.h>

namespace Web::WebAudio::Rendering {

// Normalized biquad transfer function coefficients, with a0 divided out.
// https://webaudio.github.io/web-audio-api/#filters-characteristics
struct BiquadCoefficients {
    double b0 { 0 };
    double b1 { 0 };
    double b2 { 0 };
    double a1 { 0 };
    double a2 { 0 };
};

// Computes filter coefficients from the BiquadFilterNode's computed parameter values. frequency is expected to already
// include the detune factor and be given as a fraction of the Nyquist frequency, in the range [0, 1].
WEB_API BiquadCoefficients compute_biquad_coefficients(Bindings::BiquadFilterType, double normalized_frequency, double q, double gain_db);

// Evaluates the transfer function's magnitude and phase at the given frequency, given as a fraction of the Nyquist
// frequency in the range [0, 1].
WEB_API void biquad_frequency_response(BiquadCoefficients const&, double normalized_frequency, float& magnitude, float& phase);

}
