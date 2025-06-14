/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2019-2023, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/ByteString.h>
#include <AK/FloatingPointStringConversions.h>
#include <AK/Optional.h>
#include <AK/Swift.h>
#include <AK/Vector.h>
#include <LibGfx/Color.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <ctype.h>

#ifdef LIBGFX_USE_SWIFT
#    include <LibGfx-Swift.h>
#endif

namespace Gfx {

String Color::to_string(HTMLCompatibleSerialization html_compatible_serialization) const
{
    // If the following conditions are all true:

    // 1. The color space is sRGB
    // NOTE: This is currently always true for Gfx::Color.

    // 2. The alpha is 1
    // NOTE: An alpha value of 1 will be stored as 255 currently.

    // 3. The RGB component values are internally represented as integers between 0 and 255 inclusive (i.e. 8-bit unsigned integer)
    // NOTE: This is currently always true for Gfx::Color.

    // 4. HTML-compatible serialization is requested
    if (alpha() == 255
        && html_compatible_serialization == HTMLCompatibleSerialization::Yes) {
        return MUST(String::formatted("#{:02x}{:02x}{:02x}", red(), green(), blue()));
    }

    // Otherwise, for sRGB the CSS serialization of sRGB values is used and for other color spaces, the relevant serialization of the <color> value.
    if (alpha() < 255)
        return MUST(String::formatted("rgba({}, {}, {}, {})", red(), green(), blue(), alpha() / 255.0));
    return MUST(String::formatted("rgb({}, {}, {})", red(), green(), blue()));
}

String Color::to_string_without_alpha() const
{
    return MUST(String::formatted("#{:02x}{:02x}{:02x}", red(), green(), blue()));
}

ByteString Color::to_byte_string() const
{
    return to_string().to_byte_string();
}

ByteString Color::to_byte_string_without_alpha() const
{
    return to_string_without_alpha().to_byte_string();
}

static Optional<Color> parse_rgb_color(StringView string)
{
    VERIFY(string.starts_with("rgb("_sv, CaseSensitivity::CaseInsensitive));
    VERIFY(string.ends_with(')'));

    auto substring = string.substring_view(4, string.length() - 5);
    auto parts = substring.split_view(',');

    if (parts.size() != 3)
        return {};

    auto r = parts[0].to_number<double>().map(AK::clamp_to<u8, double>);
    auto g = parts[1].to_number<double>().map(AK::clamp_to<u8, double>);
    auto b = parts[2].to_number<double>().map(AK::clamp_to<u8, double>);

    if (!r.has_value() || !g.has_value() || !b.has_value())
        return {};

    return Color(*r, *g, *b);
}

static Optional<Color> parse_rgba_color(StringView string)
{
    VERIFY(string.starts_with("rgba("_sv, CaseSensitivity::CaseInsensitive));
    VERIFY(string.ends_with(')'));

    auto substring = string.substring_view(5, string.length() - 6);
    auto parts = substring.split_view(',');

    if (parts.size() != 4)
        return {};

    auto r = parts[0].to_number<double>().map(AK::clamp_to<u8, double>);
    auto g = parts[1].to_number<double>().map(AK::clamp_to<u8, double>);
    auto b = parts[2].to_number<double>().map(AK::clamp_to<u8, double>);

    double alpha = 0;
    auto alpha_str = parts[3].trim_whitespace();
    char const* start = alpha_str.characters_without_null_termination();
    auto alpha_result = parse_first_floating_point(start, start + alpha_str.length());
    if (alpha_result.parsed_value())
        alpha = alpha_result.value;

    unsigned a = alpha * 255;

    if (!r.has_value() || !g.has_value() || !b.has_value() || a > 255)
        return {};

    return Color(*r, *g, *b, a);
}

Optional<Color> Color::from_named_css_color_string(StringView string)
{
    if (string.is_empty())
        return {};

    struct WebColor {
        ARGB32 color;
        StringView name;
    };

    constexpr Array web_colors {
        // CSS Level 1
        WebColor { 0x000000, "black"_sv },
        WebColor { 0xc0c0c0, "silver"_sv },
        WebColor { 0x808080, "gray"_sv },
        WebColor { 0xffffff, "white"_sv },
        WebColor { 0x800000, "maroon"_sv },
        WebColor { 0xff0000, "red"_sv },
        WebColor { 0x800080, "purple"_sv },
        WebColor { 0xff00ff, "fuchsia"_sv },
        WebColor { 0x008000, "green"_sv },
        WebColor { 0x00ff00, "lime"_sv },
        WebColor { 0x808000, "olive"_sv },
        WebColor { 0xffff00, "yellow"_sv },
        WebColor { 0x000080, "navy"_sv },
        WebColor { 0x0000ff, "blue"_sv },
        WebColor { 0x008080, "teal"_sv },
        WebColor { 0x00ffff, "aqua"_sv },
        // CSS Level 2 (Revision 1)
        WebColor { 0xffa500, "orange"_sv },
        // CSS Color Module Level 3
        WebColor { 0xf0f8ff, "aliceblue"_sv },
        WebColor { 0xfaebd7, "antiquewhite"_sv },
        WebColor { 0x7fffd4, "aquamarine"_sv },
        WebColor { 0xf0ffff, "azure"_sv },
        WebColor { 0xf5f5dc, "beige"_sv },
        WebColor { 0xffe4c4, "bisque"_sv },
        WebColor { 0xffebcd, "blanchedalmond"_sv },
        WebColor { 0x8a2be2, "blueviolet"_sv },
        WebColor { 0xa52a2a, "brown"_sv },
        WebColor { 0xdeb887, "burlywood"_sv },
        WebColor { 0x5f9ea0, "cadetblue"_sv },
        WebColor { 0x7fff00, "chartreuse"_sv },
        WebColor { 0xd2691e, "chocolate"_sv },
        WebColor { 0xff7f50, "coral"_sv },
        WebColor { 0x6495ed, "cornflowerblue"_sv },
        WebColor { 0xfff8dc, "cornsilk"_sv },
        WebColor { 0xdc143c, "crimson"_sv },
        WebColor { 0x00ffff, "cyan"_sv },
        WebColor { 0x00008b, "darkblue"_sv },
        WebColor { 0x008b8b, "darkcyan"_sv },
        WebColor { 0xb8860b, "darkgoldenrod"_sv },
        WebColor { 0xa9a9a9, "darkgray"_sv },
        WebColor { 0x006400, "darkgreen"_sv },
        WebColor { 0xa9a9a9, "darkgrey"_sv },
        WebColor { 0xbdb76b, "darkkhaki"_sv },
        WebColor { 0x8b008b, "darkmagenta"_sv },
        WebColor { 0x556b2f, "darkolivegreen"_sv },
        WebColor { 0xff8c00, "darkorange"_sv },
        WebColor { 0x9932cc, "darkorchid"_sv },
        WebColor { 0x8b0000, "darkred"_sv },
        WebColor { 0xe9967a, "darksalmon"_sv },
        WebColor { 0x8fbc8f, "darkseagreen"_sv },
        WebColor { 0x483d8b, "darkslateblue"_sv },
        WebColor { 0x2f4f4f, "darkslategray"_sv },
        WebColor { 0x2f4f4f, "darkslategrey"_sv },
        WebColor { 0x00ced1, "darkturquoise"_sv },
        WebColor { 0x9400d3, "darkviolet"_sv },
        WebColor { 0xff1493, "deeppink"_sv },
        WebColor { 0x00bfff, "deepskyblue"_sv },
        WebColor { 0x696969, "dimgray"_sv },
        WebColor { 0x696969, "dimgrey"_sv },
        WebColor { 0x1e90ff, "dodgerblue"_sv },
        WebColor { 0xb22222, "firebrick"_sv },
        WebColor { 0xfffaf0, "floralwhite"_sv },
        WebColor { 0x228b22, "forestgreen"_sv },
        WebColor { 0xdcdcdc, "gainsboro"_sv },
        WebColor { 0xf8f8ff, "ghostwhite"_sv },
        WebColor { 0xffd700, "gold"_sv },
        WebColor { 0xdaa520, "goldenrod"_sv },
        WebColor { 0xadff2f, "greenyellow"_sv },
        WebColor { 0x808080, "grey"_sv },
        WebColor { 0xf0fff0, "honeydew"_sv },
        WebColor { 0xff69b4, "hotpink"_sv },
        WebColor { 0xcd5c5c, "indianred"_sv },
        WebColor { 0x4b0082, "indigo"_sv },
        WebColor { 0xfffff0, "ivory"_sv },
        WebColor { 0xf0e68c, "khaki"_sv },
        WebColor { 0xe6e6fa, "lavender"_sv },
        WebColor { 0xfff0f5, "lavenderblush"_sv },
        WebColor { 0x7cfc00, "lawngreen"_sv },
        WebColor { 0xfffacd, "lemonchiffon"_sv },
        WebColor { 0xadd8e6, "lightblue"_sv },
        WebColor { 0xf08080, "lightcoral"_sv },
        WebColor { 0xe0ffff, "lightcyan"_sv },
        WebColor { 0xfafad2, "lightgoldenrodyellow"_sv },
        WebColor { 0xd3d3d3, "lightgray"_sv },
        WebColor { 0x90ee90, "lightgreen"_sv },
        WebColor { 0xd3d3d3, "lightgrey"_sv },
        WebColor { 0xffb6c1, "lightpink"_sv },
        WebColor { 0xffa07a, "lightsalmon"_sv },
        WebColor { 0x20b2aa, "lightseagreen"_sv },
        WebColor { 0x87cefa, "lightskyblue"_sv },
        WebColor { 0x778899, "lightslategray"_sv },
        WebColor { 0x778899, "lightslategrey"_sv },
        WebColor { 0xb0c4de, "lightsteelblue"_sv },
        WebColor { 0xffffe0, "lightyellow"_sv },
        WebColor { 0x32cd32, "limegreen"_sv },
        WebColor { 0xfaf0e6, "linen"_sv },
        WebColor { 0xff00ff, "magenta"_sv },
        WebColor { 0x66cdaa, "mediumaquamarine"_sv },
        WebColor { 0x0000cd, "mediumblue"_sv },
        WebColor { 0xba55d3, "mediumorchid"_sv },
        WebColor { 0x9370db, "mediumpurple"_sv },
        WebColor { 0x3cb371, "mediumseagreen"_sv },
        WebColor { 0x7b68ee, "mediumslateblue"_sv },
        WebColor { 0x00fa9a, "mediumspringgreen"_sv },
        WebColor { 0x48d1cc, "mediumturquoise"_sv },
        WebColor { 0xc71585, "mediumvioletred"_sv },
        WebColor { 0x191970, "midnightblue"_sv },
        WebColor { 0xf5fffa, "mintcream"_sv },
        WebColor { 0xffe4e1, "mistyrose"_sv },
        WebColor { 0xffe4b5, "moccasin"_sv },
        WebColor { 0xffdead, "navajowhite"_sv },
        WebColor { 0xfdf5e6, "oldlace"_sv },
        WebColor { 0x6b8e23, "olivedrab"_sv },
        WebColor { 0xff4500, "orangered"_sv },
        WebColor { 0xda70d6, "orchid"_sv },
        WebColor { 0xeee8aa, "palegoldenrod"_sv },
        WebColor { 0x98fb98, "palegreen"_sv },
        WebColor { 0xafeeee, "paleturquoise"_sv },
        WebColor { 0xdb7093, "palevioletred"_sv },
        WebColor { 0xffefd5, "papayawhip"_sv },
        WebColor { 0xffdab9, "peachpuff"_sv },
        WebColor { 0xcd853f, "peru"_sv },
        WebColor { 0xffc0cb, "pink"_sv },
        WebColor { 0xdda0dd, "plum"_sv },
        WebColor { 0xb0e0e6, "powderblue"_sv },
        WebColor { 0xbc8f8f, "rosybrown"_sv },
        WebColor { 0x4169e1, "royalblue"_sv },
        WebColor { 0x8b4513, "saddlebrown"_sv },
        WebColor { 0xfa8072, "salmon"_sv },
        WebColor { 0xf4a460, "sandybrown"_sv },
        WebColor { 0x2e8b57, "seagreen"_sv },
        WebColor { 0xfff5ee, "seashell"_sv },
        WebColor { 0xa0522d, "sienna"_sv },
        WebColor { 0x87ceeb, "skyblue"_sv },
        WebColor { 0x6a5acd, "slateblue"_sv },
        WebColor { 0x708090, "slategray"_sv },
        WebColor { 0x708090, "slategrey"_sv },
        WebColor { 0xfffafa, "snow"_sv },
        WebColor { 0x00ff7f, "springgreen"_sv },
        WebColor { 0x4682b4, "steelblue"_sv },
        WebColor { 0xd2b48c, "tan"_sv },
        WebColor { 0xd8bfd8, "thistle"_sv },
        WebColor { 0xff6347, "tomato"_sv },
        WebColor { 0x40e0d0, "turquoise"_sv },
        WebColor { 0xee82ee, "violet"_sv },
        WebColor { 0xf5deb3, "wheat"_sv },
        WebColor { 0xf5f5f5, "whitesmoke"_sv },
        WebColor { 0x9acd32, "yellowgreen"_sv },
        // CSS Color Module Level 4
        WebColor { 0x663399, "rebeccapurple"_sv },
    };

    for (auto const& web_color : web_colors) {
        if (string.equals_ignoring_ascii_case(web_color.name))
            return Color::from_rgb(web_color.color);
    }

    return {};
}

#if defined(LIBGFX_USE_SWIFT)
static Optional<Color> hex_string_to_color(StringView string)
{
    auto color = parseHexString(string);
    if (color.getCount() == 0)
        return {};
    return color[0];
}
#else
static Optional<Color> hex_string_to_color(StringView string)
{
    auto hex_nibble_to_u8 = [](char nibble) -> Optional<u8> {
        if (!isxdigit(nibble))
            return {};
        if (nibble >= '0' && nibble <= '9')
            return nibble - '0';
        return 10 + (tolower(nibble) - 'a');
    };

    if (string.length() == 4) {
        Optional<u8> r = hex_nibble_to_u8(string[1]);
        Optional<u8> g = hex_nibble_to_u8(string[2]);
        Optional<u8> b = hex_nibble_to_u8(string[3]);
        if (!r.has_value() || !g.has_value() || !b.has_value())
            return {};
        return Color(r.value() * 17, g.value() * 17, b.value() * 17);
    }

    if (string.length() == 5) {
        Optional<u8> r = hex_nibble_to_u8(string[1]);
        Optional<u8> g = hex_nibble_to_u8(string[2]);
        Optional<u8> b = hex_nibble_to_u8(string[3]);
        Optional<u8> a = hex_nibble_to_u8(string[4]);
        if (!r.has_value() || !g.has_value() || !b.has_value() || !a.has_value())
            return {};
        return Color(r.value() * 17, g.value() * 17, b.value() * 17, a.value() * 17);
    }

    if (string.length() != 7 && string.length() != 9)
        return {};

    auto to_hex = [&](char c1, char c2) -> Optional<u8> {
        auto nib1 = hex_nibble_to_u8(c1);
        auto nib2 = hex_nibble_to_u8(c2);
        if (!nib1.has_value() || !nib2.has_value())
            return {};
        return nib1.value() << 4 | nib2.value();
    };

    Optional<u8> r = to_hex(string[1], string[2]);
    Optional<u8> g = to_hex(string[3], string[4]);
    Optional<u8> b = to_hex(string[5], string[6]);
    Optional<u8> a = string.length() == 9 ? to_hex(string[7], string[8]) : Optional<u8>(255);

    if (!r.has_value() || !g.has_value() || !b.has_value() || !a.has_value())
        return {};

    return Color(r.value(), g.value(), b.value(), a.value());
}
#endif

Optional<Color> Color::from_string(StringView string)
{
    if (string.is_empty())
        return {};

    if (string[0] == '#')
        return hex_string_to_color(string);

    if (string.starts_with("rgb("_sv, CaseSensitivity::CaseInsensitive) && string.ends_with(')'))
        return parse_rgb_color(string);

    if (string.starts_with("rgba("_sv, CaseSensitivity::CaseInsensitive) && string.ends_with(')'))
        return parse_rgba_color(string);

    if (string.equals_ignoring_ascii_case("transparent"_sv))
        return Color::from_argb(0x00000000);

    if (auto const color = from_named_css_color_string(string); color.has_value())
        return color;

    return {};
}

Vector<Color> Color::shades(u32 steps, float max) const
{
    float shade = 1.f;
    float step = max / steps;
    Vector<Color> shades;
    for (u32 i = 0; i < steps; i++) {
        shade -= step;
        shades.append(this->darkened(shade));
    }
    return shades;
}

Vector<Color> Color::tints(u32 steps, float max) const
{
    float shade = 1.f;
    float step = max / steps;
    Vector<Color> tints;
    for (u32 i = 0; i < steps; i++) {
        shade += step;
        tints.append(this->lightened(shade));
    }
    return tints;
}

Color Color::from_linear_srgb(float red, float green, float blue, float alpha)
{
    auto linear_to_srgb = [](float c) {
        if (c <= 0.04045 / 12.92)
            return c * 12.92;
        return pow(c, 10. / 24) * 1.055 - 0.055;
    };

    red = linear_to_srgb(red) * 255.f;
    green = linear_to_srgb(green) * 255.f;
    blue = linear_to_srgb(blue) * 255.f;

    return Color(
        clamp(lroundf(red), 0, 255),
        clamp(lroundf(green), 0, 255),
        clamp(lroundf(blue), 0, 255),
        clamp(lroundf(alpha * 255.f), 0, 255));
}

// https://www.w3.org/TR/css-color-4/#predefined-a98-rgb
Color Color::from_a98rgb(float r, float g, float b, float alpha)
{
    auto to_linear = [](float c) {
        return pow(c, 563. / 256);
    };

    auto linear_r = to_linear(r);
    auto linear_g = to_linear(g);
    auto linear_b = to_linear(b);

    float x = 0.57666904 * linear_r + 0.18555824 * linear_g + 0.18822865 * linear_b;
    float y = 0.29734498 * linear_r + 0.62736357 * linear_g + 0.07529146 * linear_b;
    float z = 0.02703136 * linear_r + 0.07068885 * linear_g + 0.99133754 * linear_b;

    return from_xyz65(x, y, z, alpha);
}

// https://www.w3.org/TR/css-color-4/#predefined-a98-rgb
Color Color::from_display_p3(float r, float g, float b, float alpha)
{
    auto to_linear = [](float c) {
        if (c < 0.04045)
            return c / 12.92;
        return pow((c + 0.055) / (1.055), 2.4);
    };

    auto linear_r = to_linear(r);
    auto linear_g = to_linear(g);
    auto linear_b = to_linear(b);

    float x = 0.48657095 * linear_r + 0.26566769 * linear_g + 0.19821729 * linear_b;
    float y = 0.22897456 * linear_r + 0.69173852 * linear_g + 0.07928691 * linear_b;
    float z = 0.00000000 * linear_r + 0.04511338 * linear_g + 1.04394437 * linear_b;

    return from_xyz65(x, y, z, alpha);
}

// https://www.w3.org/TR/css-color-4/#predefined-prophoto-rgb
Color Color::from_pro_photo_rgb(float r, float g, float b, float alpha)
{
    auto to_linear = [](float c) -> float {
        u8 sign = c < 0 ? -1 : 1;
        float absolute = abs(c);

        if (absolute <= 16. / 252)
            return c / 16;
        return sign * pow(c, 1.8);
    };

    auto linear_r = to_linear(r);
    auto linear_g = to_linear(g);
    auto linear_b = to_linear(b);

    float x = 0.79776664 * linear_r + 0.13518130 * linear_g + 0.03134773 * linear_b;
    float y = 0.28807483 * linear_r + 0.71183523 * linear_g + 0.00008994 * linear_b;
    float z = 0.00000000 * linear_r + 0.00000000 * linear_g + 0.82510460 * linear_b;

    return from_xyz50(x, y, z, alpha);
}

// https://www.w3.org/TR/css-color-4/#predefined-rec2020
Color Color::from_rec2020(float r, float g, float b, float alpha)
{
    auto to_linear = [](float c) -> float {
        auto constexpr alpha = 1.09929682680944;
        auto constexpr beta = 0.018053968510807;

        u8 sign = c < 0 ? -1 : 1;
        auto absolute = abs(c);

        if (absolute < beta * 4.5)
            return c / 4.5;

        return sign * (pow((absolute + alpha - 1) / alpha, 1 / 0.45));
    };

    auto linear_r = to_linear(r);
    auto linear_g = to_linear(g);
    auto linear_b = to_linear(b);

    float x = 0.63695805 * linear_r + 0.14461690 * linear_g + 0.16888098 * linear_b;
    float y = 0.26270021 * linear_r + 0.67799807 * linear_g + 0.05930172 * linear_b;
    float z = 0.00000000 * linear_r + 0.02807269 * linear_g + 1.06098506 * linear_b;

    return from_xyz65(x, y, z, alpha);
}

Color Color::from_xyz50(float x, float y, float z, float alpha)
{
    // See commit description for these values.
    float r = +3.134136 * x - 1.617386 * y - 0.490662 * z;
    float g = -0.978795 * x + 1.916254 * y + 0.033443 * z;
    float b = +0.071955 * x - 0.228977 * y + 1.405386 * z;

    return from_linear_srgb(r, g, b, alpha);
}

Color Color::from_xyz65(float x, float y, float z, float alpha)
{
    // See commit description for these values.
    float r = +3.240970 * x - 1.537383 * y - 0.498611 * z;
    float g = -0.969244 * x + 1.875968 * y + 0.041555 * z;
    float b = +0.055630 * x - 0.203977 * y + 1.056972 * z;

    return from_linear_srgb(r, g, b, alpha);
}

Color Color::from_lab(float L, float a, float b, float alpha)
{
    // Third edition of "Colorimetry" by the CIE
    // 8.2.1 CIE 1976 (L*a*b*) colour space; CIELAB colour space
    float y = (L + 16) / 116;
    float x = y + a / 500;
    float z = y - b / 200;

    auto f_inv = [](float t) -> float {
        constexpr auto delta = 24. / 116;
        if (t > delta)
            return t * t * t;
        return (108. / 841) * (t - 116. / 16);
    };

    // D50
    constexpr float x_n = 0.96422;
    constexpr float y_n = 1;
    constexpr float z_n = 0.82521;

    x = x_n * f_inv(x);
    y = y_n * f_inv(y);
    z = z_n * f_inv(z);

    return from_xyz50(x, y, z, alpha);
}

}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Color const& color)
{
    return encoder.encode(color.value());
}

template<>
ErrorOr<Gfx::Color> IPC::decode(Decoder& decoder)
{
    auto rgba = TRY(decoder.decode<u32>());
    return Gfx::Color::from_argb(rgba);
}

ErrorOr<void> AK::Formatter<Gfx::Color>::format(FormatBuilder& builder, Gfx::Color value)
{
    return Formatter<StringView>::format(builder, value.to_byte_string());
}

ErrorOr<void> AK::Formatter<Gfx::YUV>::format(FormatBuilder& builder, Gfx::YUV value)
{
    return Formatter<FormatString>::format(builder, "{} {} {}"_sv, value.y, value.u, value.v);
}

ErrorOr<void> AK::Formatter<Gfx::HSV>::format(FormatBuilder& builder, Gfx::HSV value)
{
    return Formatter<FormatString>::format(builder, "{} {} {}"_sv, value.hue, value.saturation, value.value);
}

ErrorOr<void> AK::Formatter<Gfx::Oklab>::format(FormatBuilder& builder, Gfx::Oklab value)
{
    return Formatter<FormatString>::format(builder, "{} {} {}"_sv, value.L, value.a, value.b);
}
