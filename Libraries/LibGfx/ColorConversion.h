/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Math.h>
#include <math.h>

namespace Gfx {

class Color;

class ColorComponents {
public:
    constexpr ColorComponents() = default;
    constexpr ColorComponents(float first, float second, float third, float alpha = 1.0f)
        : m_components { first, second, third }
        , m_alpha(alpha)
    {
    }

    float& operator[](size_t index) { return m_components[index]; }
    float operator[](size_t index) const { return m_components[index]; }

    float alpha() const { return m_alpha; }
    void set_alpha(float alpha) { m_alpha = alpha; }

private:
    Array<float, 3> m_components { 0, 0, 0 };
    float m_alpha { 1 };
};

ColorComponents srgb_to_linear_srgb(ColorComponents const&);
ColorComponents linear_srgb_to_srgb(ColorComponents const&);

constexpr ColorComponents hsl_to_srgb(ColorComponents const& hsl)
{
    float h = fmodf(hsl[0], 360.0f);
    if (h < 0.0f)
        h += 360.0f;

    // NB: Saturation and lightness are intentionally left unclamped. Color interpolation in HSL can produce
    //     out-of-range s/l when representing colors outside the sRGB gamut, and those values should be able to
    //     round-trip back to sRGB correctly.
    float s = hsl[1];
    float l = hsl[2];
    float a = clamp(hsl.alpha(), 0.0f, 1.0f);

    // Algorithm from https://drafts.csswg.org/css-color-3/#hsl-color
    auto to_rgb = [](float h, float s, float l, float offset) {
        float k = fmodf(offset + h / 30.0f, 12.0f);
        float a = s * min(l, 1.0f - l);
        return l - a * max(-1.0f, min(min(k - 3.0f, 9.0f - k), 1.0f));
    };

    float r = to_rgb(h, s, l, 0.0f);
    float g = to_rgb(h, s, l, 8.0f);
    float b = to_rgb(h, s, l, 4.0f);

    return { r, g, b, a };
}

constexpr ColorComponents hsv_to_srgb(ColorComponents const& hsv)
{
    double hue = hsv[0];
    double saturation = hsv[1];
    double value = hsv[2];

    int high = static_cast<int>(hue / 60.0) % 6;
    double f = (hue / 60.0) - high;
    double c1 = value * (1.0 - saturation);
    double c2 = value * (1.0 - saturation * f);
    double c3 = value * (1.0 - saturation * (1.0 - f));

    double r = 0;
    double g = 0;
    double b = 0;

    switch (high) {
    case 0:
        r = value;
        g = c3;
        b = c1;
        break;
    case 1:
        r = c2;
        g = value;
        b = c1;
        break;
    case 2:
        r = c1;
        g = value;
        b = c3;
        break;
    case 3:
        r = c1;
        g = c2;
        b = value;
        break;
    case 4:
        r = c3;
        g = c1;
        b = value;
        break;
    case 5:
        r = value;
        g = c1;
        b = c2;
        break;
    }

    return { static_cast<float>(r), static_cast<float>(g), static_cast<float>(b), hsv.alpha() };
}

constexpr ColorComponents srgb_to_hsv(ColorComponents const& rgb)
{
    float r = rgb[0];
    float g = rgb[1];
    float b = rgb[2];
    float max = AK::max(AK::max(r, g), b);
    float min = AK::min(AK::min(r, g), b);
    float chroma = max - min;

    float hue = 0;
    if (chroma != 0) {
        if (max == r)
            hue = (60.0f * ((g - b) / chroma)) + 360.0f;
        else if (max == g)
            hue = (60.0f * ((b - r) / chroma)) + 120.0f;
        else
            hue = (60.0f * ((r - g) / chroma)) + 240.0f;
    }

    if (hue >= 360.0f)
        hue -= 360.0f;

    float saturation = max != 0 ? chroma / max : 0;

    return { hue, saturation, max, rgb.alpha() };
}

// https://www.itu.int/rec/R-REC-BT.1700-0-200502-I/en Table 4
constexpr ColorComponents yuv_to_srgb(ColorComponents const& yuv)
{
    float y = yuv[0];
    float u = yuv[1];
    float v = yuv[2];

    // Table 4, Items 8 and 9 arithmetically inverted
    float r = y + v / 0.877f;
    float b = y + u / 0.493f;
    float g = (y - 0.299f * r - 0.114f * b) / 0.587f;
    r = clamp(r, 0.0f, 1.0f);
    g = clamp(g, 0.0f, 1.0f);
    b = clamp(b, 0.0f, 1.0f);

    return { r, g, b, yuv.alpha() };
}

// https://www.itu.int/rec/R-REC-BT.1700-0-200502-I/en Table 4
constexpr ColorComponents srgb_to_yuv(ColorComponents const& rgb)
{
    float r = rgb[0];
    float g = rgb[1];
    float b = rgb[2];
    // Item 8
    float y = 0.299f * r + 0.587f * g + 0.114f * b;
    // Item 9
    float u = 0.493f * (b - y);
    float v = 0.877f * (r - y);
    y = clamp(y, 0.0f, 1.0f);
    u = clamp(u, -1.0f, 1.0f);
    v = clamp(v, -1.0f, 1.0f);
    return { y, u, v, rgb.alpha() };
}

// https://bottosson.github.io/posts/oklab/
constexpr ColorComponents oklab_to_linear_srgb(ColorComponents const& oklab)
{
    float L = oklab[0];
    float a = oklab[1];
    float b = oklab[2];

    float l = L + 0.3963377774f * a + 0.2158037573f * b;
    float m = L - 0.1055613458f * a - 0.0638541728f * b;
    float s = L - 0.0894841775f * a - 1.2914855480f * b;

    l = l * l * l;
    m = m * m * m;
    s = s * s * s;

    float red = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    float green = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    float blue = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;

    return { red, green, blue, oklab.alpha() };
}

// https://bottosson.github.io/posts/oklab/
constexpr ColorComponents linear_srgb_to_oklab(ColorComponents const& rgb)
{
    float r = rgb[0];
    float g = rgb[1];
    float b = rgb[2];

    float l = cbrtf(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    float m = cbrtf(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    float s = cbrtf(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);

    return {
        0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
        1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
        0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s,
        rgb.alpha(),
    };
}

ColorComponents linear_display_p3_to_xyz65(ColorComponents const&);
ColorComponents display_p3_to_linear_display_p3(ColorComponents const&);
ColorComponents a98rgb_to_xyz65(ColorComponents const&);
ColorComponents pro_photo_rgb_to_xyz50(ColorComponents const&);
ColorComponents rec2020_to_xyz65(ColorComponents const&);
ColorComponents xyz50_to_linear_srgb(ColorComponents const&);
ColorComponents xyz65_to_linear_srgb(ColorComponents const&);
ColorComponents lab_to_xyz50(ColorComponents const&);

ColorComponents color_to_srgb(Color);
Color srgb_to_color(ColorComponents const&);

ColorComponents linear_srgb_to_xyz65(ColorComponents const&);

ColorComponents xyz65_to_xyz50(ColorComponents const&);
ColorComponents xyz50_to_xyz65(ColorComponents const&);

ColorComponents xyz65_to_oklab(ColorComponents const&);
ColorComponents oklab_to_xyz65(ColorComponents const&);

ColorComponents oklab_to_oklch(ColorComponents const&);
ColorComponents oklch_to_oklab(ColorComponents const&);

ColorComponents xyz50_to_lab(ColorComponents const&);

ColorComponents lab_to_lch(ColorComponents const&);
ColorComponents lch_to_lab(ColorComponents const&);

ColorComponents srgb_to_hsl(ColorComponents const&);

ColorComponents srgb_to_hwb(ColorComponents const&);
ColorComponents hwb_to_srgb(ColorComponents const&);

ColorComponents linear_display_p3_to_display_p3(ColorComponents const&);
ColorComponents xyz65_to_linear_display_p3(ColorComponents const&);

ColorComponents a98_rgb_to_linear_a98_rgb(ColorComponents const&);
ColorComponents linear_a98_rgb_to_a98_rgb(ColorComponents const&);
ColorComponents linear_a98_rgb_to_xyz65(ColorComponents const&);
ColorComponents xyz65_to_linear_a98_rgb(ColorComponents const&);

ColorComponents prophoto_rgb_to_linear_prophoto_rgb(ColorComponents const&);
ColorComponents linear_prophoto_rgb_to_prophoto_rgb(ColorComponents const&);
ColorComponents linear_prophoto_rgb_to_xyz50(ColorComponents const&);
ColorComponents xyz50_to_linear_prophoto_rgb(ColorComponents const&);

ColorComponents rec2020_to_linear_rec2020(ColorComponents const&);
ColorComponents linear_rec2020_to_rec2020(ColorComponents const&);
ColorComponents linear_rec2020_to_xyz65(ColorComponents const&);
ColorComponents xyz65_to_linear_rec2020(ColorComponents const&);

}
