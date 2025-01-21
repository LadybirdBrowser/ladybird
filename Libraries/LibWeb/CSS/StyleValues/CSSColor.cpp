/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSColor.h"
#include <AK/TypeCasts.h>
#include <LibWeb/CSS/Serialize.h>
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

ValueComparingNonnullRefPtr<CSSColor> CSSColor::create(StringView color_space, ValueComparingNonnullRefPtr<CSSStyleValue> c1, ValueComparingNonnullRefPtr<CSSStyleValue> c2, ValueComparingNonnullRefPtr<CSSStyleValue> c3, ValueComparingRefPtr<CSSStyleValue> alpha)
{
    VERIFY(any_of(s_supported_color_space, [=](auto supported) { return color_space == supported; }));

    if (!alpha)
        alpha = NumberStyleValue::create(1);

    return adopt_ref(*new (nothrow) CSSColor(color_type_from_string_view(color_space), move(c1), move(c2), move(c3), alpha.release_nonnull()));

    VERIFY_NOT_REACHED();
}

bool CSSColor::equals(CSSStyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_color = other.as_color();
    if (color_type() != other_color.color_type())
        return false;
    auto const& other_lab_like = as<CSSColor>(other_color);
    return m_properties == other_lab_like.m_properties;
}

CSSColor::Resolved CSSColor::resolve_properties() const
{
    float const c1 = resolve_with_reference_value(m_properties.channels[0], 1).value_or(0);
    float const c2 = resolve_with_reference_value(m_properties.channels[1], 1).value_or(0);
    float const c3 = resolve_with_reference_value(m_properties.channels[2], 1).value_or(0);
    float const alpha_val = resolve_alpha(m_properties.alpha).value_or(1);
    return { .channels = { c1, c2, c3 }, .alpha = alpha_val };
}

// https://www.w3.org/TR/css-color-4/#serializing-color-function-values
String CSSColor::to_string(SerializationMode mode) const
{
    if (mode == SerializationMode::Normal) {
        auto convert_percentage = [](ValueComparingNonnullRefPtr<CSSStyleValue> const& value) -> RemoveReference<decltype(value)> {
            if (value->is_percentage())
                return NumberStyleValue::create(value->as_percentage().value() / 100);
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

    auto resolved = resolve_properties();
    if (resolved.alpha == 1) {
        return MUST(String::formatted("color({} {} {} {})",
            string_view_from_color_type(m_color_type),
            resolved.channels[0],
            resolved.channels[1],
            resolved.channels[2]));
    }

    return MUST(String::formatted("color({} {} {} {} / {})",
        string_view_from_color_type(m_color_type),
        resolved.channels[0],
        resolved.channels[1],
        resolved.channels[2],
        resolved.alpha));
}

Color CSSColor::to_color(Optional<Layout::NodeWithStyle const&>) const
{
    auto [channels, alpha_val] = resolve_properties();
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
