/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/WebAudio/Rendering/FFT.h>

namespace Web::WebAudio::Rendering {

// Radix-2 Cooley-Tukey FFT, operating in place on (re, im). `n` must be a power of two.
void radix2_fft(Span<float> re, Span<float> im)
{
    auto const n = re.size();
    VERIFY(n == im.size() && n != 0 && (n & (n - 1)) == 0);

    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            AK::swap(re[i], re[j]);
            AK::swap(im[i], im[j]);
        }
    }

    for (size_t len = 2; len <= n; len <<= 1) {
        auto half = len >> 1;
        auto angle = -2.0 * AK::Pi<double> / static_cast<double>(len);
        auto wlen_re = AK::cos(angle);
        auto wlen_im = AK::sin(angle);
        for (size_t i = 0; i < n; i += len) {
            double w_re = 1.0;
            double w_im = 0.0;
            for (size_t k = 0; k < half; ++k) {
                auto u_re = re[i + k];
                auto u_im = im[i + k];
                auto v_re = re[i + k + half] * static_cast<float>(w_re) - im[i + k + half] * static_cast<float>(w_im);
                auto v_im = re[i + k + half] * static_cast<float>(w_im) + im[i + k + half] * static_cast<float>(w_re);
                re[i + k] = u_re + v_re;
                im[i + k] = u_im + v_im;
                re[i + k + half] = u_re - v_re;
                im[i + k + half] = u_im - v_im;
                auto next_w_re = w_re * wlen_re - w_im * wlen_im;
                w_im = w_re * wlen_im + w_im * wlen_re;
                w_re = next_w_re;
            }
        }
    }
}

}
