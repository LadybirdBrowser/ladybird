/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HSLColorStyleValue.h"
#include <AK/TypeCasts.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>

namespace Web::CSS {

Optional<Color> HSLColorStyleValue::to_color(ColorResolutionContext color_resolution_context) const
{
    auto h_val = resolve_hue(m_properties.h, color_resolution_context.calculation_resolution_context);
    auto s_val = resolve_with_reference_value(m_properties.s, 100.0, color_resolution_context.calculation_resolution_context);
    auto l_val = resolve_with_reference_value(m_properties.l, 100.0, color_resolution_context.calculation_resolution_context);
    auto alpha_val = resolve_alpha(m_properties.alpha, color_resolution_context.calculation_resolution_context);

    if (!h_val.has_value() || !s_val.has_value() || !l_val.has_value() || !alpha_val.has_value())
        return {};

    return Color::from_hsla(h_val.value(), s_val.value() / 100.0f, l_val.value() / 100.0f, alpha_val.value());
}

bool HSLColorStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_color = other.as_color();
    if (color_type() != other_color.color_type())
        return false;
    auto const& other_hsl = as<HSLColorStyleValue>(other_color);
    return m_properties == other_hsl.m_properties;
}

// https://www.w3.org/TR/css-color-4/#serializing-sRGB-values
void HSLColorStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    if (auto color = to_color({}); color.has_value()) {
        builder.append(color->serialize_a_srgb_value());
        return;
    }

    builder.append("hsl("sv);
    serialize_hue_component(builder, mode, m_properties.h);
    builder.append(" "sv);
    serialize_color_component(builder, mode, m_properties.s, 100, 0);
    builder.append(" "sv);
    serialize_color_component(builder, mode, m_properties.l, 100, 0);
    if ((!m_properties.alpha->is_number() || m_properties.alpha->as_number().number() < 1) && (!m_properties.alpha->is_percentage() || m_properties.alpha->as_percentage().percentage().as_fraction() < 1)) {
        builder.append(" / "sv);
        serialize_alpha_component(builder, mode, m_properties.alpha);
    }

    builder.append(")"sv);
}

}
