/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Rendering/FFT.h>
#include <LibWeb/WebAudio/RustFFI.h>

namespace Web::WebAudio::Rendering {

FFT::FFT(size_t size)
    : m_size(size)
    , m_fft(RustFFI::web_audio_fft_create(size))
{
}

FFT::~FFT()
{
    RustFFI::web_audio_fft_destroy(m_fft);
}

void FFT::transform(Span<float> re, Span<float> im, FFTDirection direction) const
{
    static_assert(to_underlying(FFTDirection::Forward) == to_underlying(RustFFI::FFTDirection::Forward));
    static_assert(to_underlying(FFTDirection::Inverse) == to_underlying(RustFFI::FFTDirection::Inverse));
    RustFFI::web_audio_fft_transform(m_fft, re, im, static_cast<RustFFI::FFTDirection>(direction));
}

}
