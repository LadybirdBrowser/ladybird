/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ColorFunctionStyleValue.h"
#include <AK/TypeCasts.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>

namespace Web::CSS {

namespace {

CSSColorValue::ColorType color_type_from_string_view(StringView color_space)
{
    if (color_space == "a98-rgb"sv)
        return CSSColorValue::ColorType::A98RGB;
    if (color_space == "display-p3"sv)
        return CSSColorValue::ColorType::DisplayP3;
    if (color_space == "srgb"sv)
        return CSSColorValue::ColorType::sRGB;
    if (color_space == "srgb-linear"sv)
        return CSSColorValue::ColorType::sRGBLinear;
    if (color_space == "prophoto-rgb"sv)
        return CSSColorValue::ColorType::ProPhotoRGB;
    if (color_space == "rec2020"sv)
        return CSSColorValue::ColorType::Rec2020;
    if (color_space == "xyz-d50"sv)
        return CSSColorValue::ColorType::XYZD50;
    if (color_space == "xyz"sv || color_space == "xyz-d65")
        return CSSColorValue::ColorType::XYZD65;
    VERIFY_NOT_REACHED();
}

StringView string_view_from_color_type(CSSColorValue::ColorType color_type)
{
    if (color_type == CSSColorValue::ColorType::A98RGB)
        return "a98-rgb"sv;
    if (color_type == CSSColorValue::ColorType::DisplayP3)
        return "display-p3"sv;
    if (color_type == CSSColorValue::ColorType::sRGB)
        return "srgb"sv;
    if (color_type == CSSColorValue::ColorType::sRGBLinear)
        return "srgb-linear"sv;
    if (color_type == CSSColorValue::ColorType::ProPhotoRGB)
        return "prophoto-rgb"sv;
    if (color_type == CSSColorValue::ColorType::Rec2020)
        return "rec2020"sv;
    if (color_type == CSSColorValue::ColorType::XYZD50)
        return "xyz-d50"sv;
    if (color_type == CSSColorValue::ColorType::XYZD65)
        return "xyz-d65"sv;
    VERIFY_NOT_REACHED();
}

}

ValueComparingNonnullRefPtr<ColorFunctionStyleValue const> ColorFunctionStyleValue::create(StringView color_space, ValueComparingNonnullRefPtr<CSSStyleValue const> c1, ValueComparingNonnullRefPtr<CSSStyleValue const> c2, ValueComparingNonnullRefPtr<CSSStyleValue const> c3, ValueComparingRefPtr<CSSStyleValue const> alpha)
{
    VERIFY(any_of(s_supported_color_space, [=](auto supported) { return color_space == supported; }));

    if (!alpha)
        alpha = NumberStyleValue::create(1);

    return adopt_ref(*new (nothrow) ColorFunctionStyleValue(color_type_from_string_view(color_space), move(c1), move(c2), move(c3), alpha.release_nonnull()));

    VERIFY_NOT_REACHED();
}

bool ColorFunctionStyleValue::equals(CSSStyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_color = other.as_color();
    if (color_type() != other_color.color_type())
        return false;
    auto const& other_lab_like = as<ColorFunctionStyleValue>(other_color);
    return m_properties == other_lab_like.m_properties;
}

Optional<ColorFunctionStyleValue::Resolved> ColorFunctionStyleValue::resolve_properties(CalculationResolutionContext const& resolution_context) const
{
    auto c1 = resolve_with_reference_value(m_properties.channels[0], 1, resolution_context);
    auto c2 = resolve_with_reference_value(m_properties.channels[1], 1, resolution_context);
    auto c3 = resolve_with_reference_value(m_properties.channels[2], 1, resolution_context);
    auto alpha = resolve_alpha(m_properties.alpha, resolution_context);

    if (!c1.has_value() || !c2.has_value() || !c3.has_value() || !alpha.has_value())
        return {};

    float const c1_value = c1.value();
    float const c2_value = c2.value();
    float const c3_value = c3.value();
    float const alpha_value = alpha.value();

    return ColorFunctionStyleValue::Resolved { .channels = { c1_value, c2_value, c3_value }, .alpha = alpha_value };
}

// https://www.w3.org/TR/css-color-4/#serializing-color-function-values
String ColorFunctionStyleValue::to_string(SerializationMode mode) const
{
    auto convert_percentage = [&](ValueComparingNonnullRefPtr<CSSStyleValue const> const& value) -> RemoveReference<decltype(value)> {
        if (value->is_percentage())
            return NumberStyleValue::create(value->as_percentage().value() / 100);
        if (mode == SerializationMode::ResolvedValue && value->is_calculated()) {
            // FIXME: Figure out how to get the proper calculation resolution context here
            CalculationResolutionContext context {};
            auto const& calculated = value->as_calculated();
            if (calculated.resolves_to_percentage()) {
                if (auto resolved_percentage = calculated.resolve_percentage(context); resolved_percentage.has_value()) {
                    auto resolved_number = resolved_percentage->value() / 100;
                    if (!isfinite(resolved_number))
                        resolved_number = 0;
                    return NumberStyleValue::create(resolved_number);
                }
            } else if (calculated.resolves_to_number()) {
                if (auto resolved_number = calculated.resolve_number(context); resolved_number.has_value())
                    return NumberStyleValue::create(*resolved_number);
            }
        }

        return value;
    };

    auto alpha = convert_percentage(m_properties.alpha);

    bool const is_alpha_required = [&]() {
        if (alpha->is_number())
            return alpha->as_number().value() < 1;
        return true;
    }();

    if (alpha->is_number() && alpha->as_number().value() < 0)
        alpha = NumberStyleValue::create(0);

    if (is_alpha_required) {
        return MUST(String::formatted("color({} {} {} {} / {})",
            string_view_from_color_type(m_color_type),
            convert_percentage(m_properties.channels[0])->to_string(mode),
            convert_percentage(m_properties.channels[1])->to_string(mode),
            convert_percentage(m_properties.channels[2])->to_string(mode),
            alpha->to_string(mode)));
    }

    return MUST(String::formatted("color({} {} {} {})",
        string_view_from_color_type(m_color_type),
        convert_percentage(m_properties.channels[0])->to_string(mode),
        convert_percentage(m_properties.channels[1])->to_string(mode),
        convert_percentage(m_properties.channels[2])->to_string(mode)));
}

Optional<Color> ColorFunctionStyleValue::to_color(ColorResolutionContext color_resolution_context) const
{
    auto properties = resolve_properties(color_resolution_context.calculation_resolution_context);

    if (!properties.has_value())
        return {};

    auto [channels, alpha_val] = properties.value();
    auto c1 = channels[0];
    auto c2 = channels[1];
    auto c3 = channels[2];

    if (color_type() == ColorType::A98RGB)
        return Color::from_a98rgb(c1, c2, c3, alpha_val);

    if (color_type() == ColorType::DisplayP3)
        return Color::from_display_p3(c1, c2, c3, alpha_val);

    if (color_type() == ColorType::sRGB) {
        auto const to_u8 = [](float c) -> u8 { return round_to<u8>(clamp(255 * c, 0, 255)); };
        return Color(to_u8(c1), to_u8(c2), to_u8(c3), to_u8(alpha_val));
    }

    if (color_type() == ColorType::sRGBLinear)
        return Color::from_linear_srgb(c1, c2, c3, alpha_val);

    if (color_type() == ColorType::ProPhotoRGB)
        return Color::from_pro_photo_rgb(c1, c2, c3, alpha_val);

    if (color_type() == ColorType::Rec2020)
        return Color::from_rec2020(c1, c2, c3, alpha_val);

    if (color_type() == ColorType::XYZD50)
        return Color::from_xyz50(c1, c2, c3, alpha_val);

    if (color_type() == ColorType::XYZD65)
        return Color::from_xyz65(c1, c2, c3, alpha_val);

    VERIFY_NOT_REACHED();
}

} // Web::CSS
