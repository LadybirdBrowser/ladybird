/*
 * Copyright (c) 2021, Arne Elster <arne@elster.li>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Math.h>
#include <AK/Span.h>

namespace DSP {

template<size_t N>
requires(N % 2 == 0) class MDCT {
public:
    constexpr MDCT()
    {
        for (size_t n = 0; n < N; n++) {
            for (size_t k = 0; k < N / 2; k++) {
                m_phi[n][k] = AK::cos<float>(AK::Pi<float> / (2 * N) * (2 * static_cast<float>(n) + 1 + N / 2.0f) * static_cast<float>(2 * k + 1));
            }
        }
    }

    void transform(ReadonlySpan<float> data, Span<float> output)
    {
        VERIFY(N == 2 * data.size());
        VERIFY(N == output.size());
        for (size_t n = 0; n < N; n++) {
            output[n] = 0;
            for (size_t k = 0; k < N / 2; k++) {
                output[n] += data[k] * m_phi[n][k];
            }
        }
    }

private:
    Array<Array<float, N / 2>, N> m_phi;
};

}
