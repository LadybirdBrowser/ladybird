/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ColorFunctionStyleValue.h"
#include <AK/Math.h>
#include <AK/TypeCasts.h>
#include <LibGfx/ColorConversion.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<ColorFunctionStyleValue const> ColorFunctionStyleValue::create(
    ColorType color_type,
    ValueComparingNonnullRefPtr<StyleValue const> c1,
    ValueComparingNonnullRefPtr<StyleValue const> c2,
    ValueComparingNonnullRefPtr<StyleValue const> c3,
    ValueComparingRefPtr<StyleValue const> alpha,
    ColorSyntax color_syntax,
    Optional<FlyString> name,
    ValueComparingRefPtr<StyleValue const> origin_color)
{
    auto const& descriptor = color_function_descriptor_for(color_type);
    VERIFY(descriptor.serialization_behavior == SerializationBehavior::SrgbLegacy || color_syntax == ColorSyntax::Modern);

    return adopt_ref(*new (nothrow) ColorFunctionStyleValue(
        color_type, move(c1), move(c2), move(c3), move(alpha), color_syntax, move(name), move(origin_color)));
}

namespace {

struct ResolvedChannels {
    double c1 { 0 };
    double c2 { 0 };
    double c3 { 0 };
    double alpha { 0 };
};

Optional<ResolvedChannels> resolve_channels_for(ColorFunctionDescriptor const& descriptor, Array<ValueComparingNonnullRefPtr<StyleValue const>, 3> const& channels, StyleValue const* alpha_style_value, CalculationResolutionContext const& calculation_resolution_context)
{
    Array<Optional<double>, 3> resolved_channels;
    for (size_t i = 0; i < 3; ++i) {
        auto const& channel_descriptor = descriptor.channels[i];
        if (channel_descriptor.kind == ChannelKind::Hue)
            resolved_channels[i] = ColorStyleValue::resolve_hue(*channels[i], calculation_resolution_context);
        else
            resolved_channels[i] = ColorStyleValue::resolve_with_reference_value(*channels[i], channel_descriptor.percent_reference, calculation_resolution_context);
    }
    auto resolved_alpha = alpha_style_value ? ColorStyleValue::resolve_alpha(*alpha_style_value, calculation_resolution_context) : Optional<double>(1.0);

    if (!resolved_channels[0].has_value() || !resolved_channels[1].has_value() || !resolved_channels[2].has_value() || !resolved_alpha.has_value())
        return {};

    return ResolvedChannels { *resolved_channels[0], *resolved_channels[1], *resolved_channels[2], *resolved_alpha };
}

u8 clamp_to_byte(double value)
{
    if (isnan(value))
        value = 0;
    return static_cast<u8>(llround(clamp(value, 0.0, 255.0)));
}

u8 fraction_to_byte(double fraction_0_1)
{
    // Match CSS Color 4 "resolve to sRGB" rounding: round half away from zero,
    // not the default round-half-to-even that cvtsd2si uses.
    return static_cast<u8>(llround(clamp(fraction_0_1 * 255.0, 0.0, 255.0)));
}

}

Optional<Color> ColorFunctionStyleValue::to_color(ColorResolutionContext color_resolution_context) const
{
    Optional<CalculationResolutionContext> relative_resolution_context;
    RefPtr<StyleValue const> default_relative_alpha;
    if (m_origin_color) {
        auto relative_color = extract_channels_in_color_space(*m_origin_color, *color_type(), color_resolution_context);
        if (!relative_color.has_value())
            return {};
        relative_resolution_context = color_resolution_context.calculation_resolution_context;
        relative_resolution_context->relative_color = move(relative_color);
        // https://drafts.csswg.org/css-color-5/#rcs-intro
        // If the alpha value of the relative color is omitted, it defaults to that of the origin color (rather than
        // defaulting to 100%, as it does in the absolute syntax).
        if (!m_alpha)
            default_relative_alpha = KeywordStyleValue::create(Keyword::Alpha);
    }
    auto const& calculation_resolution_context = relative_resolution_context.has_value() ? *relative_resolution_context : color_resolution_context.calculation_resolution_context;

    auto resolved = resolve_channels_for(descriptor(), m_channels, m_alpha ? m_alpha.ptr() : default_relative_alpha.ptr(), calculation_resolution_context);
    if (!resolved.has_value())
        return {};

    auto [c1, c2, c3, alpha] = *resolved;

    switch (*color_type()) {
    case ColorType::RGB:
        return Color(clamp_to_byte(c1), clamp_to_byte(c2), clamp_to_byte(c3), fraction_to_byte(alpha));
    case ColorType::HSL:
        return Color::from_hsla(c1, c2 / 100.0f, c3 / 100.0f, alpha);
    case ColorType::HWB: {
        auto whiteness = static_cast<float>(clamp(c2, 0.0, 100.0) / 100.0);
        auto blackness = static_cast<float>(clamp(c3, 0.0, 100.0) / 100.0);
        if (whiteness + blackness >= 1.0f) {
            u8 gray = fraction_to_byte(whiteness / (whiteness + blackness));
            return Color(gray, gray, gray, fraction_to_byte(alpha));
        }
        auto value = 1.0f - blackness;
        auto saturation = 1.0f - (whiteness / value);
        return Color::from_hsv(c1, saturation, value).with_opacity(alpha);
    }
    case ColorType::Lab:
        return Color::from_lab(clamp(c1, 0.0, 100.0), c2, c3, alpha);
    case ColorType::OKLab:
        return Color::from_oklab(clamp(c1, 0.0, 1.0), c2, c3, alpha);
    case ColorType::LCH: {
        auto l = clamp(c1, 0.0, 100.0);
        auto hue_radians = AK::to_radians(c3);
        return Color::from_lab(l, c2 * cos(hue_radians), c2 * sin(hue_radians), alpha);
    }
    case ColorType::OKLCH: {
        auto l = clamp(c1, 0.0, 1.0);
        auto chroma = max(c2, 0.0);
        auto hue_radians = AK::to_radians(c3);
        return Color::from_oklab(l, chroma * cos(hue_radians), chroma * sin(hue_radians), alpha);
    }
    case ColorType::A98RGB:
        return Color::from_a98rgb(c1, c2, c3, alpha);
    case ColorType::DisplayP3:
        return Color::from_display_p3(c1, c2, c3, alpha);
    case ColorType::DisplayP3Linear:
        return Color::from_linear_display_p3(c1, c2, c3, alpha);
    case ColorType::sRGB: {
        auto to_u8 = [](double v) -> u8 { return round_to<u8>(clamp(255.0 * v, 0.0, 255.0)); };
        return Color(to_u8(c1), to_u8(c2), to_u8(c3), to_u8(alpha));
    }
    case ColorType::sRGBLinear:
        return Color::from_linear_srgb(c1, c2, c3, alpha);
    case ColorType::ProPhotoRGB:
        return Color::from_pro_photo_rgb(c1, c2, c3, alpha);
    case ColorType::Rec2020:
        return Color::from_rec2020(c1, c2, c3, alpha);
    case ColorType::XYZD50:
        return Color::from_xyz50(c1, c2, c3, alpha);
    case ColorType::XYZD65:
        return Color::from_xyz65(c1, c2, c3, alpha);
    }
    VERIFY_NOT_REACHED();
}

namespace {

// https://drafts.csswg.org/css-color-4/#hsl-to-rgb
ValueComparingNonnullRefPtr<StyleValue const> hsl_to_absolutized_rgb(double hue_degrees, double saturation_0_100, double lightness_0_100, double alpha_0_1)
{
    auto hue = fmod(hue_degrees, 360.0);
    if (hue < 0.0)
        hue += 360.0;
    auto saturation = clamp(saturation_0_100 / 100.0, 0.0, 1.0);
    auto lightness = clamp(lightness_0_100 / 100.0, 0.0, 1.0);

    auto to_rgb = [](double h, double s, double l, double offset) {
        auto k = fmod(offset + h / 30.0, 12.0);
        auto a = s * min(l, 1.0 - l);
        return l - a * max(-1.0, min(min(k - 3.0, 9.0 - k), 1.0));
    };

    auto r = to_rgb(hue, saturation, lightness, 0.0);
    auto g = to_rgb(hue, saturation, lightness, 8.0);
    auto b = to_rgb(hue, saturation, lightness, 4.0);

    return ColorFunctionStyleValue::create(
        ColorStyleValue::ColorType::RGB,
        NumberStyleValue::create(clamp(r * 255.0, 0, 255)),
        NumberStyleValue::create(clamp(g * 255.0, 0, 255)),
        NumberStyleValue::create(clamp(b * 255.0, 0, 255)),
        NumberStyleValue::create(clamp(alpha_0_1, 0.0, 1.0)),
        ColorSyntax::Legacy);
}

// https://drafts.csswg.org/css-color-4/#hwb-to-rgb
ValueComparingNonnullRefPtr<StyleValue const> hwb_to_absolutized_rgb(double hue_degrees, double whiteness_0_100, double blackness_0_100, double alpha_0_1)
{
    float whiteness = clamp(whiteness_0_100 / 100.0f, 0.0f, 1.0f);
    float blackness = clamp(blackness_0_100 / 100.0f, 0.0f, 1.0f);

    if (whiteness + blackness >= 1.0f) {
        auto gray = NumberStyleValue::create(clamp(whiteness / (whiteness + blackness) * 255.0f, 0.0f, 255.0f));
        return ColorFunctionStyleValue::create(
            ColorStyleValue::ColorType::RGB,
            gray, gray, gray,
            NumberStyleValue::create(clamp(alpha_0_1, 0.0, 1.0)),
            ColorSyntax::Legacy);
    }

    auto hue = fmodf(static_cast<float>(hue_degrees), 360.0f);
    if (hue < 0.0f)
        hue += 360.0f;

    auto hue_to_rgb = [](float h, float offset) {
        float k = fmodf(offset + h / 30.0f, 12.0f);
        return 0.5f - 0.5f * max(-1.0f, min(min(k - 3.0f, 9.0f - k), 1.0f));
    };

    auto scale = 1.0f - whiteness - blackness;
    auto r = hue_to_rgb(hue, 0.0f) * scale + whiteness;
    auto g = hue_to_rgb(hue, 8.0f) * scale + whiteness;
    auto b = hue_to_rgb(hue, 4.0f) * scale + whiteness;

    return ColorFunctionStyleValue::create(
        ColorStyleValue::ColorType::RGB,
        NumberStyleValue::create(clamp(r * 255.0f, 0.0f, 255.0f)),
        NumberStyleValue::create(clamp(g * 255.0f, 0.0f, 255.0f)),
        NumberStyleValue::create(clamp(b * 255.0f, 0.0f, 255.0f)),
        NumberStyleValue::create(clamp(alpha_0_1, 0.0, 1.0)),
        ColorSyntax::Legacy);
}

}

// https://drafts.csswg.org/css-color-5/#resolving-rcs
ValueComparingRefPtr<StyleValue const> ColorFunctionStyleValue::resolve_relative_form(ColorResolutionContext const& color_resolution_context) const
{
    VERIFY(m_origin_color);
    VERIFY(color_type().has_value());

    auto target_color_type = *color_type();
    auto relative_color = extract_channels_in_color_space(*m_origin_color, target_color_type, color_resolution_context);
    if (!relative_color.has_value())
        return nullptr;

    auto calculation_resolution_context = color_resolution_context.calculation_resolution_context;
    calculation_resolution_context.relative_color = move(relative_color);

    auto const& descriptor = this->descriptor();

    auto resolve_channel = [&](size_t index) -> ValueComparingNonnullRefPtr<StyleValue const> {
        auto const& value = *m_channels[index];
        if (value.to_keyword() == Keyword::None)
            return KeywordStyleValue::create(Keyword::None);
        auto const& channel_descriptor = descriptor.channels[index];
        auto resolved = channel_descriptor.kind == ChannelKind::Hue
            ? resolve_hue(value, calculation_resolution_context)
            : resolve_with_reference_value(value, channel_descriptor.percent_reference, calculation_resolution_context);
        return NumberStyleValue::create(resolved.value_or(0.0));
    };

    auto resolve_alpha_value = [&]() -> ValueComparingNonnullRefPtr<StyleValue const> {
        // https://drafts.csswg.org/css-color-5/#rcs-intro
        // If the alpha value of the relative color is omitted, it defaults to that of the origin color (rather than
        // defaulting to 100%, as it does in the absolute syntax).
        NonnullRefPtr<StyleValue const> effective_alpha = m_alpha ? *m_alpha : KeywordStyleValue::create(Keyword::Alpha);
        if (effective_alpha->to_keyword() == Keyword::None)
            return KeywordStyleValue::create(Keyword::None);
        auto resolved = resolve_alpha(*effective_alpha, calculation_resolution_context);
        return NumberStyleValue::create(resolved.value_or(1.0));
    };

    auto resolved_c1 = resolve_channel(0);
    auto resolved_c2 = resolve_channel(1);
    auto resolved_c3 = resolve_channel(2);
    auto resolved_alpha = resolve_alpha_value();

    return create(target_color_type, move(resolved_c1), move(resolved_c2), move(resolved_c3),
        move(resolved_alpha), color_syntax());
}

// https://drafts.csswg.org/css-color-4/#resolving-sRGB-values
ValueComparingNonnullRefPtr<StyleValue const> ColorFunctionStyleValue::computed_value_form() const
{
    VERIFY(!m_origin_color);
    auto color_type = *this->color_type();
    if (color_type != ColorType::RGB && color_type != ColorType::HSL && color_type != ColorType::HWB)
        return *this;

    auto number_or_zero = [](StyleValue const& value) {
        return value.is_number() ? value.as_number().number() : 0.0;
    };

    ValueComparingNonnullRefPtr<StyleValue const> alpha_value = [&]() -> ValueComparingNonnullRefPtr<StyleValue const> {
        if (!m_alpha)
            return NumberStyleValue::create(1);
        if (m_alpha->to_keyword() == Keyword::None)
            return KeywordStyleValue::create(Keyword::None);
        return NumberStyleValue::create(number_or_zero(*m_alpha));
    }();

    if (color_type == ColorType::RGB) {
        auto to_fraction = [](StyleValue const& value) -> ValueComparingNonnullRefPtr<StyleValue const> {
            if (!value.is_number())
                return KeywordStyleValue::create(Keyword::None);
            return NumberStyleValue::create(value.as_number().number() / 255.0);
        };
        return create(ColorType::sRGB,
            to_fraction(m_channels[0]), to_fraction(m_channels[1]), to_fraction(m_channels[2]),
            move(alpha_value), ColorSyntax::Modern);
    }

    Gfx::ColorComponents const native_channels {
        static_cast<float>(number_or_zero(m_channels[0])),
        static_cast<float>(number_or_zero(m_channels[1]) / 100.0),
        static_cast<float>(number_or_zero(m_channels[2]) / 100.0),
        1.0f
    };
    auto srgb = color_type == ColorType::HSL ? Gfx::hsl_to_srgb(native_channels) : Gfx::hwb_to_srgb(native_channels);
    return create(ColorType::sRGB,
        NumberStyleValue::create(srgb[0]),
        NumberStyleValue::create(srgb[1]),
        NumberStyleValue::create(srgb[2]),
        move(alpha_value), ColorSyntax::Modern);
}

ValueComparingNonnullRefPtr<StyleValue const> ColorFunctionStyleValue::absolutized(ComputationContext const& context) const
{
    auto absolutized_c1 = m_channels[0]->absolutized(context);
    auto absolutized_c2 = m_channels[1]->absolutized(context);
    auto absolutized_c3 = m_channels[2]->absolutized(context);
    ValueComparingRefPtr<StyleValue const> absolutized_alpha = m_alpha ? ValueComparingRefPtr<StyleValue const>(m_alpha->absolutized(context)) : nullptr;

    auto const& descriptor = this->descriptor();

    // https://drafts.csswg.org/css-color-5/#relative-color
    if (m_origin_color) {
        auto absolutized_origin = m_origin_color->absolutized(context);
        if (absolutized_c1 == m_channels[0] && absolutized_c2 == m_channels[1] && absolutized_c3 == m_channels[2]
            && absolutized_alpha == m_alpha && absolutized_origin == m_origin_color)
            return *this;
        return create(*color_type(), move(absolutized_c1), move(absolutized_c2), move(absolutized_c3), move(absolutized_alpha), color_syntax(), m_name, move(absolutized_origin));
    }

    if (descriptor.absolutizes_to_rgb == AbsolutizesToRgb::Yes) {
        // https://drafts.csswg.org/css-color-4/#resolving-sRGB-values
        auto c1 = descriptor.channels[0].kind == ChannelKind::Hue
            ? resolve_hue(absolutized_c1, {})
            : resolve_with_reference_value(absolutized_c1, descriptor.channels[0].percent_reference, {});
        auto c2 = resolve_with_reference_value(absolutized_c2, descriptor.channels[1].percent_reference, {});
        auto c3 = resolve_with_reference_value(absolutized_c3, descriptor.channels[2].percent_reference, {});
        auto alpha = absolutized_alpha ? resolve_alpha(*absolutized_alpha, {}) : Optional<double>(1.0);

        if (!c1.has_value() || !c2.has_value() || !c3.has_value() || !alpha.has_value())
            VERIFY_NOT_REACHED();

        if (*color_type() == ColorType::HSL)
            return hsl_to_absolutized_rgb(*c1, *c2, *c3, *alpha);
        return hwb_to_absolutized_rgb(*c1, *c2, *c3, *alpha);
    }

    if (absolutized_c1 == m_channels[0] && absolutized_c2 == m_channels[1] && absolutized_c3 == m_channels[2] && absolutized_alpha == m_alpha)
        return *this;
    return create(*color_type(), move(absolutized_c1), move(absolutized_c2), move(absolutized_c3), move(absolutized_alpha), color_syntax(), m_name);
}

bool ColorFunctionStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_color = other.as_color();
    if (color_type() != other_color.color_type())
        return false;
    auto const& other_color_function = as<ColorFunctionStyleValue>(other_color);
    if (m_channels[0] != other_color_function.m_channels[0]
        || m_channels[1] != other_color_function.m_channels[1]
        || m_channels[2] != other_color_function.m_channels[2])
        return false;
    if (m_alpha != other_color_function.m_alpha)
        return false;
    if (m_origin_color != other_color_function.m_origin_color)
        return false;
    return m_name == other_color_function.m_name;
}

namespace {

bool alpha_should_be_serialized(StyleValue const& alpha)
{
    if (alpha.is_number() && alpha.as_number().number() >= 1)
        return false;
    if (alpha.is_percentage() && alpha.as_percentage().percentage().as_fraction() >= 1)
        return false;
    return true;
}

}

void ColorFunctionStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    auto const& descriptor = this->descriptor();

    // https://drafts.csswg.org/css-color-5/#serial-relative-color
    if (m_origin_color) {
        if (descriptor.serialization_behavior == SerializationBehavior::ColorFunction) {
            builder.append("color(from "sv);
            m_origin_color->serialize(builder, mode);
            builder.appendff(" {} ", descriptor.function_name);
        } else {
            builder.appendff("{}(from ", descriptor.function_name);
            m_origin_color->serialize(builder, mode);
            builder.append(' ');
        }
        m_channels[0]->serialize(builder, mode);
        builder.append(' ');
        m_channels[1]->serialize(builder, mode);
        builder.append(' ');
        m_channels[2]->serialize(builder, mode);
        if (m_alpha) {
            builder.append(" / "sv);
            m_alpha->serialize(builder, mode);
        }
        builder.append(')');
        return;
    }

    if (descriptor.serialization_behavior == SerializationBehavior::SrgbLegacy || descriptor.serialization_behavior == SerializationBehavior::SrgbModern) {
        if (mode != SerializationMode::ResolvedValue && m_name.has_value()) {
            for (auto c : m_name->bytes_as_string_view())
                builder.append(AK::to_ascii_lowercase(c));
            return;
        }
        // sRGB-equivalent shortcut: serialize via Color::serialize_a_srgb_value when the color resolves cleanly.
        if (auto color = to_color({}); color.has_value()) {
            builder.append(color->serialize_a_srgb_value());
            return;
        }
    }

    // https://drafts.csswg.org/css-color-4/#serializing-color-function-values
    if (descriptor.serialization_behavior == SerializationBehavior::ColorFunction) {
        auto convert_percentage = [&](StyleValue const& value) -> ValueComparingNonnullRefPtr<StyleValue const> {
            if (value.is_percentage())
                return NumberStyleValue::create(value.as_percentage().raw_value() / 100);
            if (mode == SerializationMode::ResolvedValue && value.is_calculated()) {
                // FIXME: Figure out how to get the proper calculation resolution context here.
                CalculationResolutionContext calculation_resolution_context {};
                auto const& calculated = value.as_calculated();
                if (calculated.resolves_to_percentage()) {
                    if (auto resolved_percentage = calculated.resolve_percentage(calculation_resolution_context); resolved_percentage.has_value()) {
                        auto resolved_number = resolved_percentage->value() / 100;
                        if (!isfinite(resolved_number))
                            resolved_number = 0;
                        return NumberStyleValue::create(resolved_number);
                    }
                } else if (calculated.resolves_to_number()) {
                    if (auto resolved_number = calculated.resolve_number(calculation_resolution_context); resolved_number.has_value())
                        return NumberStyleValue::create(*resolved_number);
                }
            }
            return value;
        };

        // An omitted alpha is treated as 1 and not serialized.
        ValueComparingNonnullRefPtr<StyleValue const> alpha = m_alpha
            ? convert_percentage(*m_alpha)
            : NumberStyleValue::create(1);

        bool const is_alpha_required = m_alpha && [&]() {
            if (alpha->is_number())
                return alpha->as_number().number() < 1;
            return true;
        }();

        if (alpha->is_number() && alpha->as_number().number() < 0)
            alpha = NumberStyleValue::create(0);

        builder.appendff("color({} ", descriptor.function_name);
        convert_percentage(*m_channels[0])->serialize(builder, mode);
        builder.append(' ');
        convert_percentage(*m_channels[1])->serialize(builder, mode);
        builder.append(' ');
        convert_percentage(*m_channels[2])->serialize(builder, mode);
        if (is_alpha_required) {
            builder.append(" / "sv);
            alpha->serialize(builder, mode);
        }
        builder.append(')');
        return;
    }

    builder.append(descriptor.function_name);
    builder.append('(');
    for (size_t i = 0; i < 3; ++i) {
        if (i > 0)
            builder.append(' ');
        auto const& channel_descriptor = descriptor.channels[i];
        if (channel_descriptor.kind == ChannelKind::Hue)
            serialize_hue_component(builder, mode, m_channels[i]);
        else
            serialize_color_component(builder, mode, m_channels[i], channel_descriptor.percent_reference, channel_descriptor.serialize_clamp_min, channel_descriptor.serialize_clamp_max);
    }
    if (m_alpha && alpha_should_be_serialized(*m_alpha)) {
        builder.append(" / "sv);
        serialize_alpha_component(builder, mode, *m_alpha);
    }
    builder.append(')');
}

}
