/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSLCHLike.h"
#include <AK/Math.h>
#include <AK/TypeCasts.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>

namespace Web::CSS {

bool CSSLCHLike::equals(CSSStyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_color = other.as_color();
    if (color_type() != other_color.color_type())
        return false;
    auto const& other_oklch_like = as<CSSLCHLike>(other_color);
    return m_properties == other_oklch_like.m_properties;
}

Optional<Color> CSSLCH::to_color(Optional<Layout::NodeWithStyle const&>, CalculationResolutionContext const& resolution_context) const
{
    auto raw_l_val = resolve_with_reference_value(m_properties.l, 100, resolution_context);
    auto c_val = resolve_with_reference_value(m_properties.c, 150, resolution_context);
    auto raw_h_val = resolve_hue(m_properties.h, resolution_context);
    auto alpha_val = resolve_alpha(m_properties.alpha, resolution_context);

    if (!raw_l_val.has_value() || !c_val.has_value() || !raw_h_val.has_value() || !alpha_val.has_value())
        return {};

    auto l_val = clamp(raw_l_val.value(), 0, 100);
    auto h_val = AK::to_radians(raw_h_val.value());

    return Color::from_lab(l_val, c_val.value() * cos(h_val), c_val.value() * sin(h_val), alpha_val.value());
}

// https://www.w3.org/TR/css-color-4/#serializing-lab-lch
String CSSLCH::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    builder.append("lch("sv);
    serialize_color_component(builder, mode, m_properties.l, 100, 0, 100);
    builder.append(' ');
    serialize_color_component(builder, mode, m_properties.c, 150, 0, 230);
    builder.append(' ');
    serialize_hue_component(builder, mode, m_properties.h);
    if ((!m_properties.alpha->is_number() || m_properties.alpha->as_number().number() < 1)
        && (!m_properties.alpha->is_percentage() || m_properties.alpha->as_percentage().percentage().as_fraction() < 1)) {
        builder.append(" / "sv);
        serialize_alpha_component(builder, mode, m_properties.alpha);
    }

    builder.append(')');
    return MUST(builder.to_string());
}

Optional<Color> CSSOKLCH::to_color(Optional<Layout::NodeWithStyle const&>, CalculationResolutionContext const& resolution_context) const
{
    auto raw_l_val = resolve_with_reference_value(m_properties.l, 1.0, resolution_context);
    auto raw_c_val = resolve_with_reference_value(m_properties.c, 0.4, resolution_context);
    auto raw_h_val = resolve_hue(m_properties.h, resolution_context);
    auto alpha_val = resolve_alpha(m_properties.alpha, resolution_context);

    if (!raw_l_val.has_value() || !raw_c_val.has_value() || !raw_h_val.has_value() || !alpha_val.has_value())
        return {};

    auto l_val = clamp(raw_l_val.value(), 0, 1);
    auto c_val = max(raw_c_val.value(), 0);
    auto h_val = AK::to_radians(raw_h_val.value());

    return Color::from_oklab(l_val, c_val * cos(h_val), c_val * sin(h_val), alpha_val.value());
}

// https://www.w3.org/TR/css-color-4/#serializing-oklab-oklch
String CSSOKLCH::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    builder.append("oklch("sv);
    serialize_color_component(builder, mode, m_properties.l, 1.0f, 0, 1);
    builder.append(' ');
    serialize_color_component(builder, mode, m_properties.c, 0.4f, 0, 2.3);
    builder.append(' ');
    serialize_hue_component(builder, mode, m_properties.h);
    if ((!m_properties.alpha->is_number() || m_properties.alpha->as_number().number() < 1)
        && (!m_properties.alpha->is_percentage() || m_properties.alpha->as_percentage().percentage().as_fraction() < 1)) {
        builder.append(" / "sv);
        serialize_alpha_component(builder, mode, m_properties.alpha);
    }

    builder.append(')');
    return MUST(builder.to_string());
}

}
