/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSLabLike.h"
#include <AK/TypeCasts.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>

namespace Web::CSS {

bool CSSLabLike::equals(CSSStyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_color = other.as_color();
    if (color_type() != other_color.color_type())
        return false;
    auto const& other_lab_like = as<CSSLabLike>(other_color);
    return m_properties == other_lab_like.m_properties;
}

Color CSSOKLab::to_color(Optional<Layout::NodeWithStyle const&>) const
{
    auto const l_val = clamp(resolve_with_reference_value(m_properties.l, 1.0).value_or(0), 0, 1);
    auto const a_val = resolve_with_reference_value(m_properties.a, 0.4).value_or(0);
    auto const b_val = resolve_with_reference_value(m_properties.b, 0.4).value_or(0);
    auto const alpha_val = resolve_alpha(m_properties.alpha).value_or(1);

    return Color::from_oklab(l_val, a_val, b_val, alpha_val);
}

// https://www.w3.org/TR/css-color-4/#serializing-oklab-oklch
String CSSOKLab::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    builder.append("oklab("_sv);
    serialize_color_component(builder, mode, m_properties.l, 1.0f, 0, 1);
    builder.append(' ');
    serialize_color_component(builder, mode, m_properties.a, 0.4f);
    builder.append(' ');
    serialize_color_component(builder, mode, m_properties.b, 0.4f);
    if ((!m_properties.alpha->is_number() || m_properties.alpha->as_number().number() < 1)
        && (!m_properties.alpha->is_percentage() || m_properties.alpha->as_percentage().percentage().as_fraction() < 1)) {
        builder.append(" / "_sv);
        serialize_alpha_component(builder, mode, m_properties.alpha);
    }

    builder.append(')');
    return MUST(builder.to_string());
}

Color CSSLab::to_color(Optional<Layout::NodeWithStyle const&>) const
{
    auto const l_val = clamp(resolve_with_reference_value(m_properties.l, 100).value_or(0), 0, 100);
    auto const a_val = resolve_with_reference_value(m_properties.a, 125).value_or(0);
    auto const b_val = resolve_with_reference_value(m_properties.b, 125).value_or(0);
    auto const alpha_val = resolve_alpha(m_properties.alpha).value_or(1);

    return Color::from_lab(l_val, a_val, b_val, alpha_val);
}

// https://www.w3.org/TR/css-color-4/#serializing-lab-lch
String CSSLab::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    builder.append("lab("_sv);
    serialize_color_component(builder, mode, m_properties.l, 100, 0, 100);
    builder.append(' ');
    serialize_color_component(builder, mode, m_properties.a, 125);
    builder.append(' ');
    serialize_color_component(builder, mode, m_properties.b, 125);
    if ((!m_properties.alpha->is_number() || m_properties.alpha->as_number().number() < 1)
        && (!m_properties.alpha->is_percentage() || m_properties.alpha->as_percentage().percentage().as_fraction() < 1)) {
        builder.append(" / "_sv);
        serialize_alpha_component(builder, mode, m_properties.alpha);
    }

    builder.append(')');
    return MUST(builder.to_string());
}

}
