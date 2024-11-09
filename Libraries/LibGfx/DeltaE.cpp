/*
 * Copyright (c) 2023, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/Math.h>
#include <LibGfx/DeltaE.h>
#include <math.h>

namespace Gfx {

float DeltaE(CIELAB const& c1, CIELAB const& c2)
{
    // https://en.wikipedia.org/wiki/Color_difference#CIEDE2000
    // http://zschuessler.github.io/DeltaE/learn/
    // https://www.hajim.rochester.edu/ece/sites/gsharma/ciede2000/ciede2000noteCRNA.pdf

    float delta_L_prime = c2.L - c1.L;
    float L_bar = (c1.L + c2.L) / 2;

    float C1 = hypotf(c1.a, c1.b);
    float C2 = hypotf(c2.a, c2.b);
    float C_bar = (C1 + C2) / 2;

    float G = 0.5f * (1 - sqrtf(powf(C_bar, 7) / (powf(C_bar, 7) + powf(25, 7))));
    float a1_prime = (1 + G) * c1.a;
    float a2_prime = (1 + G) * c2.a;

    float C1_prime = hypotf(a1_prime, c1.b);
    float C2_prime = hypotf(a2_prime, c2.b);

    float C_prime_bar = (C1_prime + C2_prime) / 2;
    float delta_C_prime = C2_prime - C1_prime;

    auto h_prime = [](float b, float a_prime) {
        if (b == 0 && a_prime == 0)
            return 0.f;
        float h_prime = atan2(b, a_prime);
        if (h_prime < 0)
            h_prime += 2 * static_cast<float>(M_PI);
        return AK::to_degrees(h_prime);
    };
    float h1_prime = h_prime(c1.b, a1_prime);
    float h2_prime = h_prime(c2.b, a2_prime);

    float delta_h_prime;
    if (C1_prime == 0 || C2_prime == 0)
        delta_h_prime = 0;
    else if (fabsf(h1_prime - h2_prime) <= 180.f)
        delta_h_prime = h2_prime - h1_prime;
    else if (h2_prime <= h1_prime)
        delta_h_prime = h2_prime - h1_prime + 360;
    else
        delta_h_prime = h2_prime - h1_prime - 360;

    auto sin_degrees = [](float x) { return sinf(AK::to_radians(x)); };
    auto cos_degrees = [](float x) { return cosf(AK::to_radians(x)); };

    float delta_H_prime = 2 * sqrtf(C1_prime * C2_prime) * sin_degrees(delta_h_prime / 2);

    float h_prime_bar;
    if (C1_prime == 0 || C2_prime == 0)
        h_prime_bar = h1_prime + h2_prime;
    else if (fabsf(h1_prime - h2_prime) <= 180.f)
        h_prime_bar = (h1_prime + h2_prime) / 2;
    else if (h1_prime + h2_prime < 360)
        h_prime_bar = (h1_prime + h2_prime + 360) / 2;
    else
        h_prime_bar = (h1_prime + h2_prime - 360) / 2;

    float T = 1 - 0.17f * cos_degrees(h_prime_bar - 30) + 0.24f * cos_degrees(2 * h_prime_bar) + 0.32f * cos_degrees(3 * h_prime_bar + 6) - 0.2f * cos_degrees(4 * h_prime_bar - 63);

    float S_L = 1 + 0.015f * powf(L_bar - 50, 2) / sqrtf(20 + powf(L_bar - 50, 2));
    float S_C = 1 + 0.045f * C_prime_bar;
    float S_H = 1 + 0.015f * C_prime_bar * T;

    float R_T = -2 * sqrtf(powf(C_prime_bar, 7) / (powf(C_prime_bar, 7) + powf(25, 7))) * sin_degrees(60 * exp(-powf((h_prime_bar - 275) / 25, 2)));

    // "kL, kC, and kH are usually unity."
    float k_L = 1, k_C = 1, k_H = 1;

    float L = delta_L_prime / (k_L * S_L);
    float C = delta_C_prime / (k_C * S_C);
    float H = delta_H_prime / (k_H * S_H);
    return sqrtf(powf(L, 2) + powf(C, 2) + powf(H, 2) + R_T * C * H);
}

}
