/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Rendering/BiquadCoefficients.h>
#include <LibWeb/WebAudio/RustFFI.h>

namespace Web::WebAudio::Rendering {

BiquadCoefficients compute_biquad_coefficients(Bindings::BiquadFilterType type, double normalized_frequency, double q, double gain_db)
{
    static_assert(to_underlying(Bindings::BiquadFilterType::Lowpass) == to_underlying(RustFFI::BiquadFilterType::Lowpass));
    static_assert(to_underlying(Bindings::BiquadFilterType::Highpass) == to_underlying(RustFFI::BiquadFilterType::Highpass));
    static_assert(to_underlying(Bindings::BiquadFilterType::Bandpass) == to_underlying(RustFFI::BiquadFilterType::Bandpass));
    static_assert(to_underlying(Bindings::BiquadFilterType::Lowshelf) == to_underlying(RustFFI::BiquadFilterType::Lowshelf));
    static_assert(to_underlying(Bindings::BiquadFilterType::Highshelf) == to_underlying(RustFFI::BiquadFilterType::Highshelf));
    static_assert(to_underlying(Bindings::BiquadFilterType::Peaking) == to_underlying(RustFFI::BiquadFilterType::Peaking));
    static_assert(to_underlying(Bindings::BiquadFilterType::Notch) == to_underlying(RustFFI::BiquadFilterType::Notch));
    static_assert(to_underlying(Bindings::BiquadFilterType::Allpass) == to_underlying(RustFFI::BiquadFilterType::Allpass));
    auto coefficients = RustFFI::web_audio_compute_biquad_coefficients(static_cast<RustFFI::BiquadFilterType>(type), normalized_frequency, q, gain_db);
    return { coefficients.b0, coefficients.b1, coefficients.b2, coefficients.a1, coefficients.a2 };
}

void biquad_frequency_response(BiquadCoefficients const& coefficients, double normalized_frequency, float& magnitude, float& phase)
{
    RustFFI::BiquadCoefficients rust_coefficients { coefficients.b0, coefficients.b1, coefficients.b2, coefficients.a1, coefficients.a2 };
    RustFFI::web_audio_biquad_frequency_response(&rust_coefficients, normalized_frequency, &magnitude, &phase);
}

}
