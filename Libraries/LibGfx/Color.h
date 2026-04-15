/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <math.h>

#include <AK/Assertions.h>
#include <AK/Format.h>
#include <AK/Forward.h>
#include <AK/Math.h>
#include <AK/StdLibExtras.h>
#include <LibGfx/ColorConversion.h>
#include <LibIPC/Forward.h>

namespace Gfx {

// Named after in memory-order (little-endian)
// e.g. 0xAARRGGBB
using BGRA8888 = u32;

enum class AlphaType {
    Premultiplied,
    Unpremultiplied,
};

inline bool is_valid_alpha_type(u32 alpha_type)
{
    switch (alpha_type) {
    case (u32)AlphaType::Premultiplied:
    case (u32)AlphaType::Unpremultiplied:
        return true;
    }
    return false;
}

struct HSV {
    double hue { 0 };
    double saturation { 0 };
    double value { 0 };
};

struct YUV {
    float y { 0 };
    float u { 0 };
    float v { 0 };
};

struct Oklab {
    float L { 0 };
    float a { 0 };
    float b { 0 };
};

class Color {
public:
    enum class NamedColor {
        Transparent,
        Black,
        White,
        Red,
        Green,
        Cyan,
        Blue,
        Yellow,
        Magenta,
        DarkGray,
        MidGray,
        LightGray,
        WarmGray,
        DarkCyan,
        DarkGreen,
        DarkBlue,
        DarkRed,
        MidCyan,
        MidGreen,
        MidRed,
        MidBlue,
        MidMagenta,
        LightBlue,
    };

    using enum NamedColor;

    enum class BrandedColor {
        Indigo10,
        Indigo20,
        Indigo30,
        Indigo40,
        Indigo50,
        Indigo60,
        Indigo80,
        Indigo100,
        Indigo300,
        Indigo500,
        Indigo900,

        Violet10,
        Violet20,
        Violet30,
        Violet40,
        Violet50,
        Violet60,
        Violet80,
        Violet100,
        Violet300,
        Violet500,
        Violet900,

        SlateBlue10,
        SlateBlue20,
        SlateBlue30,
        SlateBlue40,
        SlateBlue50,
        SlateBlue60,
        SlateBlue80,
        SlateBlue100,
        SlateBlue300,
        SlateBlue500,
        SlateBlue900,

        Violet = Violet100,
        Indigo = Indigo100,
        SlateBlue = SlateBlue100,
    };

    constexpr Color() = default;
    constexpr Color(NamedColor);

    constexpr Color(u8 r, u8 g, u8 b)
        : m_value(0xff000000 | (r << 16) | (g << 8) | b)
    {
    }

    constexpr Color(u8 r, u8 g, u8 b, u8 a)
        : m_value((a << 24) | (r << 16) | (g << 8) | b)
    {
    }

    static constexpr Color branded_color(BrandedColor);

    static constexpr Color from_bgrx(unsigned bgrx) { return Color(bgrx | 0xff000000); }
    static constexpr Color from_bgra(unsigned bgra) { return Color(bgra); }
    static constexpr Color from_rgba(unsigned rgba)
    {
        unsigned bgra = (rgba & 0xff00ff00) | ((rgba & 0xff0000) >> 16) | ((rgba & 0xff) << 16);
        return Color::from_bgra(bgra);
    }
    static constexpr Color from_rgbx(unsigned rgbx) { return Color::from_rgba(rgbx | 0xff000000); }

    static constexpr Color from_yuv(YUV const& yuv) { return from_yuv(yuv.y, yuv.u, yuv.v); }
    static constexpr Color from_yuv(float y, float u, float v)
    {
        auto srgb = yuv_to_srgb({ y, u, v });
        return {
            static_cast<u8>(floorf(srgb[0] * 255.0f)),
            static_cast<u8>(floorf(srgb[1] * 255.0f)),
            static_cast<u8>(floorf(srgb[2] * 255.0f)),
        };
    }

    constexpr YUV to_yuv() const
    {
        auto yuv = srgb_to_yuv({ red() / 255.0f, green() / 255.0f, blue() / 255.0f });
        return { yuv[0], yuv[1], yuv[2] };
    }

    static constexpr Color from_hsl(float h_degrees, float s, float l) { return from_hsla(h_degrees, s, l, 1.0); }
    static constexpr Color from_hsla(float h_degrees, float s, float l, float a)
    {
        auto srgb = hsl_to_srgb({ h_degrees, clamp(s, 0.0f, 1.0f), clamp(l, 0.0f, 1.0f), a });
        u8 r_u8 = clamp(lroundf(srgb[0] * 255.0f), 0, 255);
        u8 g_u8 = clamp(lroundf(srgb[1] * 255.0f), 0, 255);
        u8 b_u8 = clamp(lroundf(srgb[2] * 255.0f), 0, 255);
        u8 a_u8 = clamp(lroundf(srgb.alpha() * 255.0f), 0, 255);
        return Color(r_u8, g_u8, b_u8, a_u8);
    }

    static Color from_a98rgb(float r, float g, float b, float alpha = 1.0f);
    static Color from_display_p3(float r, float g, float b, float alpha = 1.0f);
    static Color from_lab(float L, float a, float b, float alpha = 1.0f);
    static Color from_linear_display_p3(float r, float g, float b, float alpha = 1.0f);
    static Color from_linear_srgb(float r, float g, float b, float alpha = 1.0f);
    static Color from_pro_photo_rgb(float r, float g, float b, float alpha = 1.0f);
    static Color from_rec2020(float r, float g, float b, float alpha = 1.0f);
    static Color from_xyz50(float x, float y, float z, float alpha = 1.0f);
    static Color from_xyz65(float x, float y, float z, float alpha = 1.0f);

    // https://bottosson.github.io/posts/oklab/
    static constexpr Color from_oklab(float L, float a, float b, float alpha = 1.0f)
    {
        auto linear = oklab_to_linear_srgb({ L, a, b, alpha });
        return from_linear_srgb(linear[0], linear[1], linear[2], linear.alpha());
    }

    constexpr Oklab to_premultiplied_oklab()
    {
        auto oklab = to_oklab();
        return {
            oklab.L * alpha() / 255,
            oklab.a * alpha() / 255,
            oklab.b * alpha() / 255,
        };
    }

    // https://bottosson.github.io/posts/oklab/
    constexpr Oklab to_oklab()
    {
        auto linear = srgb_to_linear_srgb({ red() / 255.0f, green() / 255.0f, blue() / 255.0f });
        auto oklab = linear_srgb_to_oklab(linear);
        return { oklab[0], oklab[1], oklab[2] };
    }

    constexpr u8 red() const { return (m_value >> 16) & 0xff; }
    constexpr u8 green() const { return (m_value >> 8) & 0xff; }
    constexpr u8 blue() const { return m_value & 0xff; }
    constexpr u8 alpha() const { return (m_value >> 24) & 0xff; }

    constexpr void set_alpha(u8 value, AlphaType alpha_type = AlphaType::Unpremultiplied)
    {
        switch (alpha_type) {
        case AlphaType::Premultiplied:
            m_value = value << 24
                | (red() * value / 255) << 16
                | (green() * value / 255) << 8
                | blue() * value / 255;
            break;
        case AlphaType::Unpremultiplied:
            m_value = (m_value & 0x00ffffff) | value << 24;
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    }

    constexpr void set_red(u8 value)
    {
        m_value &= 0xff00ffff;
        m_value |= value << 16;
    }

    constexpr void set_green(u8 value)
    {
        m_value &= 0xffff00ff;
        m_value |= value << 8;
    }

    constexpr void set_blue(u8 value)
    {
        m_value &= 0xffffff00;
        m_value |= value;
    }

    constexpr Color with_alpha(u8 alpha, AlphaType alpha_type = AlphaType::Unpremultiplied) const
    {
        Color color_with_alpha = Color(m_value);
        color_with_alpha.set_alpha(alpha, alpha_type);
        return color_with_alpha;
    }

    constexpr Color blend(Color source) const
    {
        if (alpha() == 0 || source.alpha() == 255)
            return source;

        if (source.alpha() == 0)
            return *this;

        int const d = 255 * (alpha() + source.alpha()) - alpha() * source.alpha();
        u8 r = (red() * alpha() * (255 - source.alpha()) + source.red() * 255 * source.alpha()) / d;
        u8 g = (green() * alpha() * (255 - source.alpha()) + source.green() * 255 * source.alpha()) / d;
        u8 b = (blue() * alpha() * (255 - source.alpha()) + source.blue() * 255 * source.alpha()) / d;
        u8 a = d / 255;
        return Color(r, g, b, a);
    }

    ALWAYS_INLINE Color mixed_with(Color other, float weight) const
    {
        if (alpha() == other.alpha() || with_alpha(0) == other.with_alpha(0))
            return interpolate(other, weight);
        // Fallback to slower, but more visually pleasing premultiplied alpha mix.
        // This is needed for linear-gradient()s in LibWeb.
        auto mixed_alpha = mix<float>(alpha(), other.alpha(), weight);
        auto premultiplied_mix_channel = [&](float channel, float other_channel, float weight) {
            return round_to<u8>(mix<float>(channel * alpha(), other_channel * other.alpha(), weight) / mixed_alpha);
        };
        return Gfx::Color {
            premultiplied_mix_channel(red(), other.red(), weight),
            premultiplied_mix_channel(green(), other.green(), weight),
            premultiplied_mix_channel(blue(), other.blue(), weight),
            round_to<u8>(mixed_alpha),
        };
    }

    ALWAYS_INLINE Color interpolate(Color other, float weight) const noexcept
    {
        return Gfx::Color {
            round_to<u8>(mix<float>(red(), other.red(), weight)),
            round_to<u8>(mix<float>(green(), other.green(), weight)),
            round_to<u8>(mix<float>(blue(), other.blue(), weight)),
            round_to<u8>(mix<float>(alpha(), other.alpha(), weight)),
        };
    }

    constexpr Color multiply(Color other) const
    {
        return Color(
            red() * other.red() / 255,
            green() * other.green() / 255,
            blue() * other.blue() / 255,
            alpha() * other.alpha() / 255);
    }

    constexpr float distance_squared_to(Color other) const
    {
        int delta_red = other.red() - red();
        int delta_green = other.green() - green();
        int delta_blue = other.blue() - blue();
        int delta_alpha = other.alpha() - alpha();
        auto rgb_distance = (delta_red * delta_red + delta_green * delta_green + delta_blue * delta_blue) / (3.0f * 255 * 255);
        return delta_alpha * delta_alpha / (2.0f * 255 * 255) + rgb_distance * alpha() * other.alpha() / (255 * 255);
    }

    constexpr u8 luminosity() const
    {
        return round_to<u8>(red() * 0.2126f + green() * 0.7152f + blue() * 0.0722f);
    }

    constexpr float contrast_ratio(Color other)
    {
        auto l1 = luminosity();
        auto l2 = other.luminosity();
        auto darkest = min(l1, l2) / 255.;
        auto brightest = max(l1, l2) / 255.;
        return (brightest + 0.05) / (darkest + 0.05);
    }

    constexpr Color to_grayscale() const
    {
        auto gray = luminosity();
        return Color(gray, gray, gray, alpha());
    }

    constexpr Color sepia(float amount = 1.0f) const
    {
        auto blend_factor = 1.0f - amount;

        auto r1 = 0.393f + 0.607f * blend_factor;
        auto r2 = 0.769f - 0.769f * blend_factor;
        auto r3 = 0.189f - 0.189f * blend_factor;

        auto g1 = 0.349f - 0.349f * blend_factor;
        auto g2 = 0.686f + 0.314f * blend_factor;
        auto g3 = 0.168f - 0.168f * blend_factor;

        auto b1 = 0.272f - 0.272f * blend_factor;
        auto b2 = 0.534f - 0.534f * blend_factor;
        auto b3 = 0.131f + 0.869f * blend_factor;

        auto r = red();
        auto g = green();
        auto b = blue();

        return Color(
            clamp(lroundf(r * r1 + g * r2 + b * r3), 0, 255),
            clamp(lroundf(r * g1 + g * g2 + b * g3), 0, 255),
            clamp(lroundf(r * b1 + g * b2 + b * b3), 0, 255),
            alpha());
    }

    constexpr Color with_opacity(float opacity) const
    {
        VERIFY(opacity >= 0 && opacity <= 1);
        return with_alpha(static_cast<u8>(round(alpha() * opacity)));
    }

    constexpr Color darkened(float amount = 0.5f) const
    {
        return Color(red() * amount, green() * amount, blue() * amount, alpha());
    }

    constexpr Color lightened(float amount = 1.2f) const
    {
        return Color(min(255, (int)((float)red() * amount)), min(255, (int)((float)green() * amount)), min(255, (int)((float)blue() * amount)), alpha());
    }

    Vector<Color> shades(u32 steps, float max = 1.f) const;
    Vector<Color> tints(u32 steps, float max = 1.f) const;

    constexpr Color saturated_to(float saturation) const
    {
        auto hsv = to_hsv();
        auto alpha = this->alpha();
        auto color = Color::from_hsv(hsv.hue, static_cast<double>(saturation), hsv.value);
        color.set_alpha(alpha);
        return color;
    }

    constexpr Color inverted() const
    {
        return Color(~red(), ~green(), ~blue(), alpha());
    }

    constexpr Color xored(Color other) const
    {
        return Color(((other.m_value ^ m_value) & 0x00ffffff) | (m_value & 0xff000000));
    }

    constexpr BGRA8888 value() const { return m_value; }

    constexpr bool operator==(Color other) const
    {
        return m_value == other.m_value;
    }

    enum class HTMLCompatibleSerialization {
        No,
        Yes,
    };

    [[nodiscard]] String to_string(HTMLCompatibleSerialization = HTMLCompatibleSerialization::No) const;
    String to_string_without_alpha() const;
    Utf16String to_utf16_string_without_alpha() const;

    void serialize_a_srgb_value(StringBuilder&) const;
    String serialize_a_srgb_value() const;

    ByteString to_byte_string() const;
    ByteString to_byte_string_without_alpha() const;
    static Optional<Color> from_string(StringView);
    static Optional<Color> from_utf16_string(Utf16View const&);
    static Optional<Color> from_named_css_color_string(StringView);

    constexpr HSV to_hsv() const
    {
        auto hsv = srgb_to_hsv({ red() / 255.0f, green() / 255.0f, blue() / 255.0f });

        VERIFY(hsv[0] >= 0.0f && hsv[0] < 360.0f);
        VERIFY(hsv[1] >= 0.0f && hsv[1] <= 1.0f);
        VERIFY(hsv[2] >= 0.0f && hsv[2] <= 1.0f);

        return { hsv[0], hsv[1], hsv[2] };
    }

    static constexpr Color from_hsv(double hue, double saturation, double value)
    {
        return from_hsv({ hue, saturation, value });
    }

    static constexpr Color from_hsv(HSV const& hsv)
    {
        VERIFY(hsv.hue >= 0.0 && hsv.hue < 360.0);
        VERIFY(hsv.saturation >= 0.0 && hsv.saturation <= 1.0);
        VERIFY(hsv.value >= 0.0 && hsv.value <= 1.0);

        auto srgb = hsv_to_srgb({ static_cast<float>(hsv.hue), static_cast<float>(hsv.saturation), static_cast<float>(hsv.value) });
        auto out_r = static_cast<u8>(round(srgb[0] * 255));
        auto out_g = static_cast<u8>(round(srgb[1] * 255));
        auto out_b = static_cast<u8>(round(srgb[2] * 255));
        return Color(out_r, out_g, out_b);
    }

    constexpr Color suggested_foreground_color() const
    {
        return luminosity() < 128 ? Color::White : Color::Black;
    }

private:
    constexpr explicit Color(BGRA8888 argb)
        : m_value(argb)
    {
    }

    BGRA8888 m_value { 0 };
};

constexpr Color::Color(NamedColor named)
{
    if (named == Transparent) {
        m_value = 0;
        return;
    }

    struct {
        u8 r;
        u8 g;
        u8 b;
    } rgb;

    switch (named) {
    case Black:
        rgb = { 0, 0, 0 };
        break;
    case White:
        rgb = { 255, 255, 255 };
        break;
    case Red:
        rgb = { 255, 0, 0 };
        break;
    case Green:
        rgb = { 0, 255, 0 };
        break;
    case Cyan:
        rgb = { 0, 255, 255 };
        break;
    case DarkCyan:
        rgb = { 0, 127, 127 };
        break;
    case MidCyan:
        rgb = { 0, 192, 192 };
        break;
    case Blue:
        rgb = { 0, 0, 255 };
        break;
    case Yellow:
        rgb = { 255, 255, 0 };
        break;
    case Magenta:
        rgb = { 255, 0, 255 };
        break;
    case DarkGray:
        rgb = { 64, 64, 64 };
        break;
    case MidGray:
        rgb = { 127, 127, 127 };
        break;
    case LightGray:
        rgb = { 192, 192, 192 };
        break;
    case MidGreen:
        rgb = { 0, 192, 0 };
        break;
    case MidBlue:
        rgb = { 0, 0, 192 };
        break;
    case MidRed:
        rgb = { 192, 0, 0 };
        break;
    case MidMagenta:
        rgb = { 192, 0, 192 };
        break;
    case DarkGreen:
        rgb = { 0, 128, 0 };
        break;
    case DarkBlue:
        rgb = { 0, 0, 128 };
        break;
    case DarkRed:
        rgb = { 128, 0, 0 };
        break;
    case WarmGray:
        rgb = { 212, 208, 200 };
        break;
    case LightBlue:
        rgb = { 173, 216, 230 };
        break;
    default:
        VERIFY_NOT_REACHED();
        break;
    }

    m_value = 0xff000000 | (rgb.r << 16) | (rgb.g << 8) | rgb.b;
}

constexpr Color Color::branded_color(BrandedColor color)
{
    // clang-format off
    switch (color) {
    case BrandedColor::Indigo10:     return from_bgrx(0xa5'a6'f2);
    case BrandedColor::Indigo20:     return from_bgrx(0x8a'88'eb);
    case BrandedColor::Indigo30:     return from_bgrx(0x68'51'd6);
    case BrandedColor::Indigo40:     return from_bgrx(0x55'3f'c4);
    case BrandedColor::Indigo50:     return from_bgrx(0x4d'37'b8);
    case BrandedColor::Indigo60:     return from_bgrx(0x3c'28'a1);
    case BrandedColor::Indigo80:     return from_bgrx(0x30'1f'82);
    case BrandedColor::Indigo100:    return from_bgrx(0x2a'13'73);
    case BrandedColor::Indigo300:    return from_bgrx(0x26'0f'73);
    case BrandedColor::Indigo500:    return from_bgrx(0x1d'0c'59);
    case BrandedColor::Indigo900:    return from_bgrx(0x19'0c'4a);

    case BrandedColor::Violet10:     return from_bgrx(0xe0'd4'ff);
    case BrandedColor::Violet20:     return from_bgrx(0xca'b5'ff);
    case BrandedColor::Violet30:     return from_bgrx(0xc3'ab'ff);
    case BrandedColor::Violet40:     return from_bgrx(0xb4'96'ff);
    case BrandedColor::Violet50:     return from_bgrx(0xab'8e'f5);
    case BrandedColor::Violet60:     return from_bgrx(0x9d'7c'f2);
    case BrandedColor::Violet80:     return from_bgrx(0x93'6f'ed);
    case BrandedColor::Violet100:    return from_bgrx(0x8a'64'e5);
    case BrandedColor::Violet300:    return from_bgrx(0x82'57'e6);
    case BrandedColor::Violet500:    return from_bgrx(0x7a'4c'e6);
    case BrandedColor::Violet900:    return from_bgrx(0x6a'39'db);

    case BrandedColor::SlateBlue10:  return from_bgrx(0xcb'e0'f7);
    case BrandedColor::SlateBlue20:  return from_bgrx(0xc1'd9'f5);
    case BrandedColor::SlateBlue30:  return from_bgrx(0xb6'd2'f2);
    case BrandedColor::SlateBlue40:  return from_bgrx(0xa8'c8'ed);
    case BrandedColor::SlateBlue50:  return from_bgrx(0x97'bc'e6);
    case BrandedColor::SlateBlue60:  return from_bgrx(0x86'ad'd9);
    case BrandedColor::SlateBlue80:  return from_bgrx(0x77'a1'd1);
    case BrandedColor::SlateBlue100: return from_bgrx(0x6d'98'cc);
    case BrandedColor::SlateBlue300: return from_bgrx(0x5c'8e'cc);
    case BrandedColor::SlateBlue500: return from_bgrx(0x54'84'bf);
    case BrandedColor::SlateBlue900: return from_bgrx(0x48'72'a3);
    }
    // clang-format on

    VERIFY_NOT_REACHED();
}

}

using Gfx::Color;

namespace AK {

template<>
class Traits<Color> : public DefaultTraits<Color> {
public:
    static unsigned hash(Color const& color)
    {
        return u32_hash(color.value());
    }
};

template<>
struct Formatter<Gfx::Color> : public Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder&, Gfx::Color);
};

template<>
struct Formatter<Gfx::YUV> : public Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder&, Gfx::YUV);
};

template<>
struct Formatter<Gfx::HSV> : public Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder&, Gfx::HSV);
};

template<>
struct Formatter<Gfx::Oklab> : public Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder&, Gfx::Oklab);
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::Color const&);

template<>
ErrorOr<Gfx::Color> decode(Decoder&);

}
