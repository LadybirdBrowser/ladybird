/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/WebAudio/Rendering/FFT.h>

namespace Web::WebAudio::Rendering {

// Radix-2 Cooley-Tukey FFT of a fixed power-of-two size. The twiddle factors depend only on that size, so they are
// computed once here and shared by every transform.
FFT::FFT(size_t size)
    : m_size(size)
{
    VERIFY(size != 0 && (size & (size - 1)) == 0);

    m_twiddle_real = MUST(FixedArray<float>::create(size / 2));
    m_twiddle_imag = MUST(FixedArray<float>::create(size / 2));
    for (size_t index = 0; index < size / 2; ++index) {
        auto angle = -2. * AK::Pi<double> * static_cast<double>(index) / static_cast<double>(size);
        m_twiddle_real[index] = static_cast<float>(AK::cos(angle));
        m_twiddle_imag[index] = static_cast<float>(AK::sin(angle));
    }
}

// Both spans must hold exactly `size()` samples. The inverse transform includes the 1/N scaling.
void FFT::transform(Span<float> re, Span<float> im, FFTDirection direction) const
{
    VERIFY(re.size() == m_size && im.size() == m_size);

    for (size_t i = 1, j = 0; i < m_size; ++i) {
        size_t bit = m_size >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            AK::swap(re[i], re[j]);
            AK::swap(im[i], im[j]);
        }
    }

    auto twiddle_sign = direction == FFTDirection::Forward ? 1.f : -1.f;
    for (size_t len = 2; len <= m_size; len <<= 1) {
        auto half = len >> 1;
        auto twiddle_stride = m_size / len;
        for (size_t i = 0; i < m_size; i += len) {
            for (size_t k = 0; k < half; ++k) {
                auto w_re = m_twiddle_real[k * twiddle_stride];
                auto w_im = m_twiddle_imag[k * twiddle_stride] * twiddle_sign;
                auto u_re = re[i + k];
                auto u_im = im[i + k];
                auto v_re = re[i + k + half] * w_re - im[i + k + half] * w_im;
                auto v_im = re[i + k + half] * w_im + im[i + k + half] * w_re;
                re[i + k] = u_re + v_re;
                im[i + k] = u_im + v_im;
                re[i + k + half] = u_re - v_re;
                im[i + k + half] = u_im - v_im;
            }
        }
    }

    if (direction == FFTDirection::Inverse) {
        auto inverse_size = 1.f / static_cast<float>(m_size);
        for (size_t i = 0; i < m_size; ++i) {
            re[i] *= inverse_size;
            im[i] *= inverse_size;
        }
    }
}

}
