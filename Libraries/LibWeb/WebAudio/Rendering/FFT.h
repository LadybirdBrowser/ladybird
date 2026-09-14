/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FixedArray.h>
#include <AK/Span.h>

namespace Web::WebAudio::Rendering {

enum class FFTDirection {
    Forward,
    Inverse,
};

class FFT {
public:
    explicit FFT(size_t size);

    size_t size() const { return m_size; }

    void transform(Span<float> re, Span<float> im, FFTDirection = FFTDirection::Forward) const;

private:
    size_t m_size { 0 };
    FixedArray<float> m_twiddle_real;
    FixedArray<float> m_twiddle_imag;
};

}
