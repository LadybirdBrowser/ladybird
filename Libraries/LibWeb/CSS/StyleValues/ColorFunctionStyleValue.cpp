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

}
