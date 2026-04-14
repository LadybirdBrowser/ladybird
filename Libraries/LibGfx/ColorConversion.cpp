/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ColorConversion.h>
#include <math.h>

namespace Gfx {

// https://drafts.csswg.org/css-color-4/#predefined-sRGB
ColorComponents srgb_to_linear_srgb(ColorComponents const& srgb)
{
    auto to_linear = [](float c) {
        if (c >= 0.04045f)
            return static_cast<float>(pow((c + 0.055f) / 1.055f, 2.4f));
        return c / 12.92f;
    };

    return { to_linear(srgb[0]), to_linear(srgb[1]), to_linear(srgb[2]), srgb.alpha() };
}

// https://drafts.csswg.org/css-color-4/#predefined-sRGB
ColorComponents linear_srgb_to_srgb(ColorComponents const& linear)
{
    auto to_srgb = [](float c) {
        if (c <= 0.04045f / 12.92f)
            return static_cast<float>(c * 12.92f);
        return static_cast<float>(pow(c, 10.0 / 24.0) * 1.055 - 0.055);
    };

    return { to_srgb(linear[0]), to_srgb(linear[1]), to_srgb(linear[2]), linear.alpha() };
}

// https://drafts.csswg.org/css-color-4/#predefined-display-p3
ColorComponents linear_display_p3_to_xyz65(ColorComponents const& p3)
{
    float x = 0.48657095f * p3[0] + 0.26566769f * p3[1] + 0.19821729f * p3[2];
    float y = 0.22897456f * p3[0] + 0.69173852f * p3[1] + 0.07928691f * p3[2];
    float z = 0.00000000f * p3[0] + 0.04511338f * p3[1] + 1.04394437f * p3[2];

    return { x, y, z, p3.alpha() };
}

// https://drafts.csswg.org/css-color-4/#predefined-display-p3
ColorComponents display_p3_to_linear_display_p3(ColorComponents const& p3)
{
    auto to_linear = [](float c) {
        if (c < 0.04045f)
            return static_cast<float>(c / 12.92f);
        return static_cast<float>(pow((c + 0.055f) / 1.055f, 2.4));
    };

    return { to_linear(p3[0]), to_linear(p3[1]), to_linear(p3[2]), p3.alpha() };
}

// https://drafts.csswg.org/css-color-4/#predefined-a98-rgb
ColorComponents a98rgb_to_xyz65(ColorComponents const& a98)
{
    auto to_linear = [](float c) {
        return static_cast<float>(pow(c, 563.0 / 256.0));
    };

    auto linear_r = to_linear(a98[0]);
    auto linear_g = to_linear(a98[1]);
    auto linear_b = to_linear(a98[2]);

    float x = 0.57666904f * linear_r + 0.18555824f * linear_g + 0.18822865f * linear_b;
    float y = 0.29734498f * linear_r + 0.62736357f * linear_g + 0.07529146f * linear_b;
    float z = 0.02703136f * linear_r + 0.07068885f * linear_g + 0.99133754f * linear_b;

    return { x, y, z, a98.alpha() };
}

// https://drafts.csswg.org/css-color-4/#predefined-prophoto-rgb
ColorComponents pro_photo_rgb_to_xyz50(ColorComponents const& prophoto)
{
    auto to_linear = [](float c) -> float {
        float sign = c < 0 ? -1.0f : 1.0f;
        float absolute = abs(c);

        if (absolute <= 16.0f / 252.0f)
            return c / 16.0f;
        return sign * static_cast<float>(pow(c, 1.8));
    };

    auto linear_r = to_linear(prophoto[0]);
    auto linear_g = to_linear(prophoto[1]);
    auto linear_b = to_linear(prophoto[2]);

    float x = 0.79776664f * linear_r + 0.13518130f * linear_g + 0.03134773f * linear_b;
    float y = 0.28807483f * linear_r + 0.71183523f * linear_g + 0.00008994f * linear_b;
    float z = 0.00000000f * linear_r + 0.00000000f * linear_g + 0.82510460f * linear_b;

    return { x, y, z, prophoto.alpha() };
}

// https://drafts.csswg.org/css-color-4/#predefined-rec2020
ColorComponents rec2020_to_xyz65(ColorComponents const& rec2020)
{
    auto to_linear = [](float c) -> float {
        auto constexpr alpha = 1.09929682680944;
        auto constexpr beta = 0.018053968510807;

        float sign = c < 0 ? -1.0f : 1.0f;
        auto absolute = abs(c);

        if (absolute < beta * 4.5)
            return static_cast<float>(c / 4.5);

        return sign * static_cast<float>(pow((absolute + alpha - 1) / alpha, 1 / 0.45));
    };

    auto linear_r = to_linear(rec2020[0]);
    auto linear_g = to_linear(rec2020[1]);
    auto linear_b = to_linear(rec2020[2]);

    float x = 0.63695805f * linear_r + 0.14461690f * linear_g + 0.16888098f * linear_b;
    float y = 0.26270021f * linear_r + 0.67799807f * linear_g + 0.05930172f * linear_b;
    float z = 0.00000000f * linear_r + 0.02807269f * linear_g + 1.06098506f * linear_b;

    return { x, y, z, rec2020.alpha() };
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

ColorComponents lab_to_xyz50(ColorComponents const& lab)
{
    // Third edition of "Colorimetry" by the CIE
    // 8.2.1 CIE 1976 (L*a*b*) colour space; CIELAB colour space
    float L = lab[0];
    float a = lab[1];
    float b = lab[2];

    float fy = (L + 16.0f) / 116.0f;
    float fx = fy + a / 500.0f;
    float fz = fy - b / 200.0f;

    auto f_inv = [](float t) -> float {
        constexpr auto delta = 24.0 / 116.0;
        if (t > delta)
            return t * t * t;
        return static_cast<float>((108.0 / 841.0) * (t - 116.0 / 16.0));
    };

    // D50
    constexpr float x_n = 0.96422f;
    constexpr float y_n = 1.0f;
    constexpr float z_n = 0.82521f;

    float x = x_n * f_inv(fx);
    float y = y_n * f_inv(fy);
    float z = z_n * f_inv(fz);

    return { x, y, z, lab.alpha() };
}

}
