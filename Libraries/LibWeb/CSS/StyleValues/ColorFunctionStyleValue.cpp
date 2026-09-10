/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ColorFunctionStyleValue.h"
#include <AK/Math.h>
#include <LibGfx/ColorConversion.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>

namespace Web::CSS {

ColorFunctionStyleValue::ColorFunctionStyleValue(StyleValueFFI::StyleValueData const* data)
    : ColorStyleValue(data)
{
}

ValueComparingNonnullRefPtr<ColorFunctionStyleValue const> ColorFunctionStyleValue::create(
    ColorType color_type,
    ValueComparingNonnullRefPtr<StyleValue const> c1,
    ValueComparingNonnullRefPtr<StyleValue const> c2,
    ValueComparingNonnullRefPtr<StyleValue const> c3,
    ValueComparingRefPtr<StyleValue const> alpha,
    ColorSyntax color_syntax,
    Optional<Utf16FlyString> name,
    ValueComparingRefPtr<StyleValue const> origin_color)
{
    auto const& descriptor = color_function_descriptor_for(color_type);
    VERIFY(descriptor.serialization_behavior == SerializationBehavior::SrgbLegacy || color_syntax == ColorSyntax::Modern);

    return adopt_ref(*new (nothrow) ColorFunctionStyleValue(
        color_type, move(c1), move(c2), move(c3), move(alpha), color_syntax, move(name), move(origin_color)));
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
    VERIFY(origin_color());
    VERIFY(color_type().has_value());

    auto target_color_type = *color_type();
    auto relative_color = extract_channels_in_color_space(*origin_color(), target_color_type, color_resolution_context);
    if (!relative_color.has_value())
        return nullptr;

    auto calculation_resolution_context = color_resolution_context.calculation_resolution_context;
    calculation_resolution_context.relative_color = move(relative_color);

    auto const& descriptor = this->descriptor();

    auto resolve_channel = [&](size_t index) -> ValueComparingNonnullRefPtr<StyleValue const> {
        auto value = channel(index);
        if (value->to_keyword() == Keyword::None)
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
        NonnullRefPtr<StyleValue const> effective_alpha = alpha() ? *alpha() : KeywordStyleValue::create(Keyword::Alpha);
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
    VERIFY(!origin_color());
    auto color_type = *this->color_type();
    if (color_type != ColorType::RGB && color_type != ColorType::HSL && color_type != ColorType::HWB)
        return *this;

    auto number_or_zero = [](StyleValue const& value) {
        return value.is_number() ? value.as_number().number() : 0.0;
    };

    ValueComparingNonnullRefPtr<StyleValue const> alpha_value = [&]() -> ValueComparingNonnullRefPtr<StyleValue const> {
        if (!alpha())
            return NumberStyleValue::create(1);
        if (alpha()->to_keyword() == Keyword::None)
            return KeywordStyleValue::create(Keyword::None);
        return NumberStyleValue::create(number_or_zero(*alpha()));
    }();

    if (color_type == ColorType::RGB) {
        auto to_fraction = [](StyleValue const& value) -> ValueComparingNonnullRefPtr<StyleValue const> {
            if (!value.is_number())
                return KeywordStyleValue::create(Keyword::None);
            return NumberStyleValue::create(value.as_number().number() / 255.0);
        };
        return create(ColorType::sRGB,
            to_fraction(channels()[0]), to_fraction(channels()[1]), to_fraction(channels()[2]),
            move(alpha_value), ColorSyntax::Modern);
    }

    Gfx::ColorComponents const native_channels {
        static_cast<float>(number_or_zero(channels()[0])),
        static_cast<float>(number_or_zero(channels()[1]) / 100.0),
        static_cast<float>(number_or_zero(channels()[2]) / 100.0),
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
    auto absolutized_c1 = channels()[0]->absolutized(context);
    auto absolutized_c2 = channels()[1]->absolutized(context);
    auto absolutized_c3 = channels()[2]->absolutized(context);
    ValueComparingRefPtr<StyleValue const> absolutized_alpha = alpha() ? ValueComparingRefPtr<StyleValue const>(alpha()->absolutized(context)) : nullptr;

    auto const& descriptor = this->descriptor();

    // https://drafts.csswg.org/css-color-5/#relative-color
    if (origin_color()) {
        auto absolutized_origin = origin_color()->absolutized(context);
        if (absolutized_c1 == channels()[0] && absolutized_c2 == channels()[1] && absolutized_c3 == channels()[2]
            && absolutized_alpha == alpha() && absolutized_origin == origin_color())
            return *this;
        return create(*color_type(), move(absolutized_c1), move(absolutized_c2), move(absolutized_c3), move(absolutized_alpha), color_syntax(), name(), move(absolutized_origin));
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

    if (absolutized_c1 == channels()[0] && absolutized_c2 == channels()[1] && absolutized_c3 == channels()[2] && absolutized_alpha == alpha())
        return *this;
    return create(*color_type(), move(absolutized_c1), move(absolutized_c2), move(absolutized_c3), move(absolutized_alpha), color_syntax(), name());
}

}
