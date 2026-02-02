/*
 * Copyright (c) 2024-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LCHLikeColorStyleValue.h"
#include <AK/Math.h>
#include <AK/TypeCasts.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>

namespace Web::CSS {

bool LCHLikeColorStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_color = other.as_color();
    if (color_type() != other_color.color_type())
        return false;
    auto const& other_oklch_like = as<LCHLikeColorStyleValue>(other_color);
    return m_properties == other_oklch_like.m_properties;
}

Optional<Color> LCHColorStyleValue::to_color(ColorResolutionContext color_resolution_context) const
{
    auto raw_l_val = resolve_with_reference_value(m_properties.l, 100, color_resolution_context.calculation_resolution_context);
    auto c_val = resolve_with_reference_value(m_properties.c, 150, color_resolution_context.calculation_resolution_context);
    auto raw_h_val = resolve_hue(m_properties.h, color_resolution_context.calculation_resolution_context);
    auto alpha_val = resolve_alpha(m_properties.alpha, color_resolution_context.calculation_resolution_context);

    if (!raw_l_val.has_value() || !c_val.has_value() || !raw_h_val.has_value() || !alpha_val.has_value())
        return {};

    auto l_val = clamp(raw_l_val.value(), 0, 100);
    auto h_val = AK::to_radians(raw_h_val.value());

    return Color::from_lab(l_val, c_val.value() * cos(h_val), c_val.value() * sin(h_val), alpha_val.value());
}

ValueComparingNonnullRefPtr<StyleValue const> LCHColorStyleValue::absolutized(ComputationContext const& context) const
{
    auto l = m_properties.l->absolutized(context);
    auto c = m_properties.c->absolutized(context);
    auto h = m_properties.h->absolutized(context);
    auto alpha = m_properties.alpha->absolutized(context);
    if (l == m_properties.l && c == m_properties.c && h == m_properties.h && alpha == m_properties.alpha)
        return *this;
    return LCHLikeColorStyleValue::create<LCHColorStyleValue>(move(l), move(c), move(h), move(alpha));
}

// https://www.w3.org/TR/css-color-4/#serializing-lab-lch
void LCHColorStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
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
}

Optional<Color> OKLCHColorStyleValue::to_color(ColorResolutionContext color_resolution_context) const
{
    auto raw_l_val = resolve_with_reference_value(m_properties.l, 1.0, color_resolution_context.calculation_resolution_context);
    auto raw_c_val = resolve_with_reference_value(m_properties.c, 0.4, color_resolution_context.calculation_resolution_context);
    auto raw_h_val = resolve_hue(m_properties.h, color_resolution_context.calculation_resolution_context);
    auto alpha_val = resolve_alpha(m_properties.alpha, color_resolution_context.calculation_resolution_context);

    if (!raw_l_val.has_value() || !raw_c_val.has_value() || !raw_h_val.has_value() || !alpha_val.has_value())
        return {};

    auto l_val = clamp(raw_l_val.value(), 0, 1);
    auto c_val = max(raw_c_val.value(), 0);
    auto h_val = AK::to_radians(raw_h_val.value());

    return Color::from_oklab(l_val, c_val * cos(h_val), c_val * sin(h_val), alpha_val.value());
}

ValueComparingNonnullRefPtr<StyleValue const> OKLCHColorStyleValue::absolutized(ComputationContext const& context) const
{
    auto l = m_properties.l->absolutized(context);
    auto c = m_properties.c->absolutized(context);
    auto h = m_properties.h->absolutized(context);
    auto alpha = m_properties.alpha->absolutized(context);
    if (l == m_properties.l && c == m_properties.c && h == m_properties.h && alpha == m_properties.alpha)
        return *this;
    return LCHLikeColorStyleValue::create<OKLCHColorStyleValue>(l, c, h, alpha);
}

// https://www.w3.org/TR/css-color-4/#serializing-oklab-oklch
void OKLCHColorStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
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
}

}
