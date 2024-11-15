/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSColor.h"
#include <AK/TypeCasts.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>

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
    auto const& other_lab_like = verify_cast<CSSColor>(other_color);
    return m_properties == other_lab_like.m_properties;
}

// https://www.w3.org/TR/css-color-4/#serializing-color-function-values
String CSSColor::to_string() const
{
    // FIXME: Do this properly, taking unresolved calculated values into account.
    return serialize_a_srgb_value(to_color({}));
}

Color CSSColor::to_color(Optional<Layout::NodeWithStyle const&>) const
{
    auto const c1 = resolve_with_reference_value(m_properties.channels[0], 1).value_or(0);
    auto const c2 = resolve_with_reference_value(m_properties.channels[1], 1).value_or(0);
    auto const c3 = resolve_with_reference_value(m_properties.channels[2], 1).value_or(0);
    auto const alpha_val = resolve_alpha(m_properties.alpha).value_or(1);

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
