/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibGfx/Color.h>
#include <LibGfx/ColorConversion.h>
#include <math.h>

namespace Gfx {

ColorComponents color_to_srgb(Color color)
{
    return {
        color.red() / 255.0f,
        color.green() / 255.0f,
        color.blue() / 255.0f,
        color.alpha() / 255.0f,
    };
}

Color srgb_to_color(ColorComponents const& components)
{
    return Color(
        clamp(lroundf(components[0] * 255.0f), 0L, 255L),
        clamp(lroundf(components[1] * 255.0f), 0L, 255L),
        clamp(lroundf(components[2] * 255.0f), 0L, 255L),
        clamp(lroundf(components.alpha() * 255.0f), 0L, 255L));
}

// https://drafts.csswg.org/css-color-4/#predefined-sRGB
// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents srgb_to_linear_srgb(ColorComponents const& srgb)
{
    auto to_linear = [](float c) {
        float sign = c < 0 ? -1.0f : 1.0f;
        float absolute = abs(c);

        if (absolute <= 0.04045f)
            return c / 12.92f;

        return sign * static_cast<float>(pow((absolute + 0.055f) / 1.055f, 2.4));
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
            return sign * static_cast<float>(1.055 * pow(absolute, 1.0 / 2.4) - 0.055);

        return 12.92f * c;
    };

    return { to_srgb(linear[0]), to_srgb(linear[1]), to_srgb(linear[2]), linear.alpha() };
}

// https://drafts.csswg.org/css-color-4/#predefined-display-p3
// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents linear_display_p3_to_xyz65(ColorComponents const& p3)
{
    float x = 0.48657095f * p3[0] + 0.26566769f * p3[1] + 0.19821729f * p3[2];
    float y = 0.22897456f * p3[0] + 0.69173852f * p3[1] + 0.07928691f * p3[2];
    float z = 0.00000000f * p3[0] + 0.04511338f * p3[1] + 1.04394437f * p3[2];

    return { x, y, z, p3.alpha() };
}

// https://drafts.csswg.org/css-color-4/#predefined-display-p3
// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents display_p3_to_linear_display_p3(ColorComponents const& p3)
{
    auto to_linear = [](float c) {
        float sign = c < 0 ? -1.0f : 1.0f;
        float absolute = abs(c);

        if (absolute <= 0.04045f)
            return c / 12.92f;

        return sign * static_cast<float>(pow((absolute + 0.055f) / 1.055f, 2.4));
    };

    return { to_linear(p3[0]), to_linear(p3[1]), to_linear(p3[2]), p3.alpha() };
}

// https://drafts.csswg.org/css-color-4/#predefined-a98-rgb
// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents a98rgb_to_xyz65(ColorComponents const& a98)
{
    auto to_linear = [](float c) {
        float sign = c < 0 ? -1.0f : 1.0f;
        float absolute = abs(c);

        return sign * static_cast<float>(pow(absolute, 563.0 / 256.0));
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
// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents pro_photo_rgb_to_xyz50(ColorComponents const& prophoto)
{
    auto to_linear = [](float c) -> float {
        float sign = c < 0 ? -1.0f : 1.0f;
        float absolute = abs(c);

        if (absolute <= 16.0f / 512.0f)
            return c / 16.0f;

        return sign * static_cast<float>(pow(absolute, 1.8));
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
// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents rec2020_to_xyz65(ColorComponents const& rec2020)
{
    auto to_linear = [](float c) -> float {
        constexpr auto alpha = 1.09929682680944;
        constexpr auto beta = 0.018053968510807;

        float sign = c < 0 ? -1.0f : 1.0f;
        float absolute = abs(c);

        if (absolute < beta * 4.5)
            return c / 4.5f;

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
    float y = L > kappa * epsilon ? static_cast<float>(pow((L + 16.0) / 116.0, 3)) : static_cast<float>(L / kappa);
    float z = compute(f2);

    // D50
    constexpr float x_n = 0.3457f / 0.3585f;
    constexpr float y_n = 1.0f;
    constexpr float z_n = (1.0f - 0.3457f - 0.3585f) / 0.3585f;

    return { x_n * x, y_n * y, z_n * z, lab.alpha() };
}

// https://drafts.csswg.org/css-color-4/#predefined-sRGB-linear
// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents linear_srgb_to_xyz65(ColorComponents const& components)
{
    float red = components[0];
    float green = components[1];
    float blue = components[2];
    return {
        0.4123907993f * red + 0.3575843394f * green + 0.1804807884f * blue,
        0.2126390059f * red + 0.7151686788f * green + 0.0721923154f * blue,
        0.0193308187f * red + 0.1191947798f * green + 0.9505321522f * blue,
        components.alpha(),
    };
}

// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents xyz65_to_xyz50(ColorComponents const& components)
{
    float x = components[0];
    float y = components[1];
    float z = components[2];
    return {
        +1.0479298208f * x + 0.0229468746f * y - 0.0501922295f * z,
        +0.0296278156f * x + 0.9904344482f * y - 0.0170738250f * z,
        -0.0092430581f * x + 0.0150551448f * y + 0.7518742814f * z,
        components.alpha(),
    };
}

// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents xyz50_to_xyz65(ColorComponents const& components)
{
    float x = components[0];
    float y = components[1];
    float z = components[2];
    return {
        +0.9554734528f * x - 0.0230985368f * y + 0.0632593086f * z,
        -0.0283697094f * x + 1.0099954580f * y + 0.0210415381f * z,
        +0.0123140016f * x - 0.0205076964f * y + 1.3303659366f * z,
        components.alpha(),
    };
}

// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents xyz65_to_oklab(ColorComponents const& components)
{
    float x = components[0];
    float y = components[1];
    float z = components[2];

    float long_cone = cbrtf(0.8190224379967030f * x + 0.3619062600528904f * y - 0.1288737815209879f * z);
    float medium_cone = cbrtf(0.0329836539323885f * x + 0.9292868615863434f * y + 0.0361446663506424f * z);
    float short_cone = cbrtf(0.0481771893596242f * x + 0.2642395317527308f * y + 0.6335478284694309f * z);

    return {
        0.2104542683093140f * long_cone + 0.7936177747023054f * medium_cone - 0.0040720430116193f * short_cone,
        1.9779985324311684f * long_cone - 2.4285922420485799f * medium_cone + 0.4505937096174110f * short_cone,
        0.0259040424655478f * long_cone + 0.7827717124575296f * medium_cone - 0.8086757549230774f * short_cone,
        components.alpha(),
    };
}

// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents oklab_to_xyz65(ColorComponents const& components)
{
    float lightness = components[0];
    float a = components[1];
    float b = components[2];

    float long_cone = lightness + 0.3963377774f * a + 0.2158037573f * b;
    float medium_cone = lightness - 0.1055613458f * a - 0.0638541728f * b;
    float short_cone = lightness - 0.0894841775f * a - 1.2914855480f * b;

    long_cone = long_cone * long_cone * long_cone;
    medium_cone = medium_cone * medium_cone * medium_cone;
    short_cone = short_cone * short_cone * short_cone;

    return {
        +1.2268798758459243f * long_cone - 0.5578149944602171f * medium_cone + 0.2813910456659647f * short_cone,
        -0.0405757452148008f * long_cone + 1.1122868032803170f * medium_cone - 0.0717110580655164f * short_cone,
        -0.0763729366746601f * long_cone - 0.4214933324022432f * medium_cone + 1.5869240198367816f * short_cone,
        components.alpha(),
    };
}

static ColorComponents rectangular_to_polar(ColorComponents const& components)
{
    float a = components[1];
    float b = components[2];

    float chroma = sqrtf(a * a + b * b);
    float hue = atan2f(b, a) * 180.0f / AK::Pi<float>;
    if (hue < 0.0f)
        hue += 360.0f;

    return { components[0], chroma, hue, components.alpha() };
}

static ColorComponents polar_to_rectangular(ColorComponents const& components)
{
    float chroma = components[1];
    float hue_radians = components[2] * AK::Pi<float> / 180.0f;

    return {
        components[0],
        chroma * cosf(hue_radians),
        chroma * sinf(hue_radians),
        components.alpha(),
    };
}

// https://drafts.csswg.org/css-color-4/#lab-to-lch
ColorComponents oklab_to_oklch(ColorComponents const& components) { return rectangular_to_polar(components); }

// https://drafts.csswg.org/css-color-4/#lch-to-lab
ColorComponents oklch_to_oklab(ColorComponents const& components) { return polar_to_rectangular(components); }

// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents xyz50_to_lab(ColorComponents const& components)
{
    constexpr auto kappa = 24389.0 / 27.0;
    constexpr auto epsilon = 216.0 / 24389.0;

    // D50
    constexpr float x_n = 0.3457f / 0.3585f;
    constexpr float y_n = 1.0f;
    constexpr float z_n = (1.0f - 0.3457f - 0.3585f) / 0.3585f;

    auto f = [](float value) -> float {
        if (value > epsilon)
            return cbrtf(value);
        return static_cast<float>((kappa * value + 16.0) / 116.0);
    };

    float fx = f(components[0] / x_n);
    float fy = f(components[1] / y_n);
    float fz = f(components[2] / z_n);

    return {
        116.0f * fy - 16.0f,
        500.0f * (fx - fy),
        200.0f * (fy - fz),
        components.alpha(),
    };
}

// https://drafts.csswg.org/css-color-4/#lab-to-lch
ColorComponents lab_to_lch(ColorComponents const& components) { return rectangular_to_polar(components); }

// https://drafts.csswg.org/css-color-4/#lch-to-lab
ColorComponents lch_to_lab(ColorComponents const& components) { return polar_to_rectangular(components); }

// https://drafts.csswg.org/css-color-4/#rgb-to-hsl
ColorComponents srgb_to_hsl(ColorComponents const& components)
{
    float red = components[0];
    float green = components[1];
    float blue = components[2];

    float maximum = max(max(red, green), blue);
    float minimum = min(min(red, green), blue);
    float chroma = maximum - minimum;
    float lightness = (minimum + maximum) / 2.0f;

    float hue = 0.0f;
    float saturation = 0.0f;

    if (chroma != 0.0f) {
        if (lightness == 0.0f || lightness == 1.0f)
            saturation = 0.0f;
        else
            saturation = (maximum - lightness) / min(lightness, 1.0f - lightness);

        if (maximum == red)
            hue = (green - blue) / chroma + (green < blue ? 6.0f : 0.0f);
        else if (maximum == green)
            hue = (blue - red) / chroma + 2.0f;
        else
            hue = (red - green) / chroma + 4.0f;
        hue *= 60.0f;

        if (saturation < 0.0f) {
            hue += 180.0f;
            saturation = fabsf(saturation);
        }

        if (hue >= 360.0f)
            hue -= 360.0f;
    }

    return { hue, saturation, lightness, components.alpha() };
}

// https://drafts.csswg.org/css-color-4/#hwb-to-rgb
ColorComponents hwb_to_srgb(ColorComponents const& components)
{
    float hue = components[0];
    float whiteness = components[1];
    float blackness = components[2];

    if (whiteness + blackness >= 1.0f) {
        float gray = whiteness / (whiteness + blackness);
        return { gray, gray, gray, components.alpha() };
    }

    auto rgb = hsl_to_srgb({ hue, 1.0f, 0.5f, components.alpha() });
    float scale = 1.0f - whiteness - blackness;

    return {
        rgb[0] * scale + whiteness,
        rgb[1] * scale + whiteness,
        rgb[2] * scale + whiteness,
        components.alpha(),
    };
}

// https://drafts.csswg.org/css-color-4/#rgb-to-hwb
ColorComponents srgb_to_hwb(ColorComponents const& components)
{
    float red = components[0];
    float green = components[1];
    float blue = components[2];

    float maximum = max(max(red, green), blue);
    float minimum = min(min(red, green), blue);
    float chroma = maximum - minimum;

    float hue = 0.0f;
    if (chroma != 0.0f) {
        if (maximum == red)
            hue = ((green - blue) / chroma) + (green < blue ? 6.0f : 0.0f);
        else if (maximum == green)
            hue = (blue - red) / chroma + 2.0f;
        else
            hue = (red - green) / chroma + 4.0f;
        hue *= 60.0f;
        if (hue >= 360.0f)
            hue -= 360.0f;
    }

    return { hue, minimum, 1.0f - maximum, components.alpha() };
}

// https://drafts.csswg.org/css-color-4/#predefined-display-p3
// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents linear_display_p3_to_display_p3(ColorComponents const& components)
{
    auto to_gamma = [](float c) {
        float sign = c < 0 ? -1.0f : 1.0f;
        float absolute = abs(c);

        if (absolute > 0.0031308f)
            return sign * static_cast<float>(1.055 * pow(absolute, 1.0 / 2.4) - 0.055);

        return 12.92f * c;
    };

    return { to_gamma(components[0]), to_gamma(components[1]), to_gamma(components[2]), components.alpha() };
}

// https://drafts.csswg.org/css-color-4/#predefined-display-p3-linear
// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents xyz65_to_linear_display_p3(ColorComponents const& components)
{
    float x = components[0];
    float y = components[1];
    float z = components[2];
    return {
        +2.4934969119f * x - 0.9313836179f * y - 0.4027107845f * z,
        -0.8294889696f * x + 1.7626640603f * y + 0.0236246858f * z,
        +0.0358458302f * x - 0.0761723893f * y + 0.9568845240f * z,
        components.alpha(),
    };
}

// https://drafts.csswg.org/css-color-4/#predefined-a98-rgb
// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents a98_rgb_to_linear_a98_rgb(ColorComponents const& components)
{
    auto to_linear = [](float c) {
        float sign = c < 0.0f ? -1.0f : 1.0f;
        return sign * static_cast<float>(pow(abs(c), 563.0 / 256.0));
    };

    return { to_linear(components[0]), to_linear(components[1]), to_linear(components[2]), components.alpha() };
}

// https://drafts.csswg.org/css-color-4/#predefined-a98-rgb
// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents linear_a98_rgb_to_a98_rgb(ColorComponents const& components)
{
    auto to_gamma = [](float c) {
        float sign = c < 0.0f ? -1.0f : 1.0f;
        return sign * static_cast<float>(pow(abs(c), 256.0 / 563.0));
    };

    return { to_gamma(components[0]), to_gamma(components[1]), to_gamma(components[2]), components.alpha() };
}

// https://drafts.csswg.org/css-color-4/#predefined-a98-rgb
// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents linear_a98_rgb_to_xyz65(ColorComponents const& components)
{
    float red = components[0];
    float green = components[1];
    float blue = components[2];
    return {
        0.57666904f * red + 0.18555824f * green + 0.18822865f * blue,
        0.29734498f * red + 0.62736357f * green + 0.07529146f * blue,
        0.02703136f * red + 0.07068885f * green + 0.99133754f * blue,
        components.alpha(),
    };
}

// https://drafts.csswg.org/css-color-4/#predefined-a98-rgb
// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents xyz65_to_linear_a98_rgb(ColorComponents const& components)
{
    float x = components[0];
    float y = components[1];
    float z = components[2];
    return {
        +2.0415879038f * x - 0.5650069743f * y - 0.3473784579f * z,
        -0.9692436363f * x + 1.8759675015f * y + 0.0415550574f * z,
        +0.0134442806f * x - 0.1183623922f * y + 1.0151749944f * z,
        components.alpha(),
    };
}

// https://drafts.csswg.org/css-color-4/#predefined-prophoto-rgb
// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents prophoto_rgb_to_linear_prophoto_rgb(ColorComponents const& components)
{
    auto to_linear = [](float c) -> float {
        float sign = c < 0.0f ? -1.0f : 1.0f;
        float absolute = abs(c);
        if (absolute <= 16.0f / 512.0f)
            return c / 16.0f;
        return sign * static_cast<float>(pow(absolute, 1.8));
    };

    return { to_linear(components[0]), to_linear(components[1]), to_linear(components[2]), components.alpha() };
}

// https://drafts.csswg.org/css-color-4/#predefined-prophoto-rgb
// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents linear_prophoto_rgb_to_prophoto_rgb(ColorComponents const& components)
{
    auto to_gamma = [](float c) -> float {
        float sign = c < 0.0f ? -1.0f : 1.0f;
        float absolute = abs(c);
        if (absolute <= 1.0f / 512.0f)
            return c * 16.0f;
        return sign * static_cast<float>(pow(absolute, 1.0 / 1.8));
    };

    return { to_gamma(components[0]), to_gamma(components[1]), to_gamma(components[2]), components.alpha() };
}

// https://drafts.csswg.org/css-color-4/#predefined-prophoto-rgb
// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents linear_prophoto_rgb_to_xyz50(ColorComponents const& components)
{
    float red = components[0];
    float green = components[1];
    float blue = components[2];
    return {
        0.79776664f * red + 0.13518130f * green + 0.03134773f * blue,
        0.28807483f * red + 0.71183523f * green + 0.00008994f * blue,
        0.00000000f * red + 0.00000000f * green + 0.82510460f * blue,
        components.alpha(),
    };
}

// https://drafts.csswg.org/css-color-4/#predefined-prophoto-rgb
// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents xyz50_to_linear_prophoto_rgb(ColorComponents const& components)
{
    float x = components[0];
    float y = components[1];
    float z = components[2];
    return {
        +1.3457989731f * x - 0.2555802200f * y - 0.0511188540f * z,
        -0.5446224939f * x + 1.5082327413f * y + 0.0205274474f * z,
        +0.0000000000f * x + 0.0000000000f * y + 1.2119675456f * z,
        components.alpha(),
    };
}

// https://drafts.csswg.org/css-color-4/#predefined-rec2020
// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents rec2020_to_linear_rec2020(ColorComponents const& components)
{
    auto to_linear = [](float c) -> float {
        float sign = c < 0.0f ? -1.0f : 1.0f;
        float absolute = abs(c);

        return sign * static_cast<float>(pow(absolute, 2.4));
    };

    return { to_linear(components[0]), to_linear(components[1]), to_linear(components[2]), components.alpha() };
}

// https://drafts.csswg.org/css-color-4/#predefined-rec2020
// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents linear_rec2020_to_rec2020(ColorComponents const& components)
{
    auto to_gamma = [](float c) -> float {
        float sign = c < 0.0f ? -1.0f : 1.0f;
        float absolute = abs(c);

        return sign * static_cast<float>(pow(absolute, 1.0 / 2.4));
    };

    return { to_gamma(components[0]), to_gamma(components[1]), to_gamma(components[2]), components.alpha() };
}

// https://drafts.csswg.org/css-color-4/#predefined-rec2020
// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents linear_rec2020_to_xyz65(ColorComponents const& components)
{
    float red = components[0];
    float green = components[1];
    float blue = components[2];
    return {
        0.63695805f * red + 0.14461690f * green + 0.16888098f * blue,
        0.26270021f * red + 0.67799807f * green + 0.05930172f * blue,
        0.00000000f * red + 0.02807269f * green + 1.06098506f * blue,
        components.alpha(),
    };
}

// https://drafts.csswg.org/css-color-4/#predefined-rec2020
// https://drafts.csswg.org/css-color-4/#color-conversion-code
ColorComponents xyz65_to_linear_rec2020(ColorComponents const& components)
{
    float x = components[0];
    float y = components[1];
    float z = components[2];
    return {
        +1.7166511880f * x - 0.3556707838f * y - 0.2533662814f * z,
        -0.6666843518f * x + 1.6164812366f * y + 0.0157685458f * z,
        +0.0176398574f * x - 0.0427706133f * y + 0.9421031212f * z,
        components.alpha(),
    };
}

}
