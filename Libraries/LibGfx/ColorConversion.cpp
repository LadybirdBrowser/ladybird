/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibGfx/ColorConversion.h>
#include <math.h>

namespace Gfx {

// https://drafts.csswg.org/css-color-4/#predefined-sRGB
// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents srgb_to_linear_srgb(ColorComponents const& srgb)
{
    auto to_linear = [](float c) {
        double sign = c < 0 ? -1.0 : 1.0;
        double absolute = abs(c);

        if (absolute <= 0.04045)
            return static_cast<float>(c / 12.92);

        return static_cast<float>(sign * AK::pow<double>((absolute + 0.055) / 1.055, 2.4));
    };

    return { to_linear(srgb[0]), to_linear(srgb[1]), to_linear(srgb[2]), srgb.alpha() };
}

// https://drafts.csswg.org/css-color-4/#predefined-sRGB
// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents linear_srgb_to_srgb(ColorComponents const& linear)
{
    auto to_srgb = [](float c) {
        float sign = c < 0 ? -1.0f : 1.0f;
        float absolute = abs(c);

        if (absolute > 0.0031308f)
            return sign * static_cast<float>(1.055 * AK::pow<double>(absolute, 1.0 / 2.4) - 0.055);

        return 12.92f * c;
    };

    return { to_srgb(linear[0]), to_srgb(linear[1]), to_srgb(linear[2]), linear.alpha() };
}

ColorComponents xyz50_to_linear_srgb(ColorComponents const& xyz)
{
    // See commit description for these values.
    float r = +3.134136f * xyz[0] - 1.617386f * xyz[1] - 0.490662f * xyz[2];
    float g = -0.978795f * xyz[0] + 1.916254f * xyz[1] + 0.033443f * xyz[2];
    float b = +0.071955f * xyz[0] - 0.228977f * xyz[1] + 1.405386f * xyz[2];

    return { r, g, b, xyz.alpha() };
}

ColorComponents xyz65_to_linear_srgb(ColorComponents const& xyz)
{
    // See commit description for these values.
    float r = +3.240970f * xyz[0] - 1.537383f * xyz[1] - 0.498611f * xyz[2];
    float g = -0.969244f * xyz[0] + 1.875968f * xyz[1] + 0.041555f * xyz[2];
    float b = +0.055630f * xyz[0] - 0.203977f * xyz[1] + 1.056972f * xyz[2];

    return { r, g, b, xyz.alpha() };
}

// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents lab_to_xyz50(ColorComponents const& lab)
{
    constexpr auto kappa = 24389.0 / 27.0;
    constexpr auto epsilon = 216.0 / 24389.0;

    float L = lab[0];
    float a = lab[1];
    float b = lab[2];

    float f1 = (L + 16.0f) / 116.0f;
    float f0 = a / 500.0f + f1;
    float f2 = f1 - b / 200.0f;

    auto compute = [](float f) -> float {
        float cubed = f * f * f;
        if (cubed > epsilon)
            return cubed;
        return static_cast<float>((116.0 * f - 16.0) / kappa);
    };

    float x = compute(f0);
    float y = L > kappa * epsilon ? static_cast<float>(AK::pow<double>((L + 16.0) / 116.0, 3)) : static_cast<float>(L / kappa);
    float z = compute(f2);

    // D50
    constexpr float x_n = 0.3457f / 0.3585f;
    constexpr float y_n = 1.0f;
    constexpr float z_n = (1.0f - 0.3457f - 0.3585f) / 0.3585f;

    return { x_n * x, y_n * y, z_n * z, lab.alpha() };
}

}
