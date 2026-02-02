/*
 * Copyright (c) 2024-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LabLikeColorStyleValue.h"
#include <AK/TypeCasts.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

bool LabLikeColorStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_color = other.as_color();
    if (color_type() != other_color.color_type())
        return false;
    auto const& other_lab_like = as<LabLikeColorStyleValue>(other_color);
    return m_properties == other_lab_like.m_properties;
}

Optional<Color> OKLabColorStyleValue::to_color(ColorResolutionContext color_resolution_context) const
{
    auto const l_val = resolve_with_reference_value(m_properties.l, 1.0, color_resolution_context.calculation_resolution_context);
    auto const a_val = resolve_with_reference_value(m_properties.a, 0.4, color_resolution_context.calculation_resolution_context);
    auto const b_val = resolve_with_reference_value(m_properties.b, 0.4, color_resolution_context.calculation_resolution_context);
    auto const alpha_val = resolve_alpha(m_properties.alpha, color_resolution_context.calculation_resolution_context);

    if (!l_val.has_value() || !a_val.has_value() || !b_val.has_value() || !alpha_val.has_value())
        return {};

    return Color::from_oklab(clamp(l_val.value(), 0, 1), a_val.value(), b_val.value(), alpha_val.value());
}

ValueComparingNonnullRefPtr<StyleValue const> OKLabColorStyleValue::absolutized(ComputationContext const& context) const
{
    auto l = m_properties.l->absolutized(context);
    auto a = m_properties.a->absolutized(context);
    auto b = m_properties.b->absolutized(context);
    auto alpha = m_properties.alpha->absolutized(context);
    if (l == m_properties.l && a == m_properties.a && b == m_properties.b && alpha == m_properties.alpha)
        return *this;
    return LabLikeColorStyleValue::create<OKLabColorStyleValue>(move(l), move(a), move(b), move(alpha));
}

// https://www.w3.org/TR/css-color-4/#serializing-oklab-oklch
void OKLabColorStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    builder.append("oklab("sv);
    serialize_color_component(builder, mode, m_properties.l, 1.0f, 0, 1);
    builder.append(' ');
    serialize_color_component(builder, mode, m_properties.a, 0.4f);
    builder.append(' ');
    serialize_color_component(builder, mode, m_properties.b, 0.4f);
    if ((!m_properties.alpha->is_number() || m_properties.alpha->as_number().number() < 1)
        && (!m_properties.alpha->is_percentage() || m_properties.alpha->as_percentage().percentage().as_fraction() < 1)) {
        builder.append(" / "sv);
        serialize_alpha_component(builder, mode, m_properties.alpha);
    }

    builder.append(')');
}

Optional<Color> LabColorStyleValue::to_color(ColorResolutionContext color_resolution_context) const
{
    auto l_val = resolve_with_reference_value(m_properties.l, 100, color_resolution_context.calculation_resolution_context);
    auto a_val = resolve_with_reference_value(m_properties.a, 125, color_resolution_context.calculation_resolution_context);
    auto b_val = resolve_with_reference_value(m_properties.b, 125, color_resolution_context.calculation_resolution_context);
    auto alpha_val = resolve_alpha(m_properties.alpha, color_resolution_context.calculation_resolution_context);

    if (!l_val.has_value() || !a_val.has_value() || !b_val.has_value() || !alpha_val.has_value())
        return {};

    return Color::from_lab(clamp(l_val.value(), 0, 100), a_val.value(), b_val.value(), alpha_val.value());
}

ValueComparingNonnullRefPtr<StyleValue const> LabColorStyleValue::absolutized(ComputationContext const& context) const
{
    auto l = m_properties.l->absolutized(context);
    auto a = m_properties.a->absolutized(context);
    auto b = m_properties.b->absolutized(context);
    auto alpha = m_properties.alpha->absolutized(context);
    if (l == m_properties.l && a == m_properties.a && b == m_properties.b && alpha == m_properties.alpha)
        return *this;
    return LabLikeColorStyleValue::create<LabColorStyleValue>(l, a, b, alpha);
}

// https://www.w3.org/TR/css-color-4/#serializing-lab-lch
void LabColorStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    builder.append("lab("sv);
    serialize_color_component(builder, mode, m_properties.l, 100, 0, 100);
    builder.append(' ');
    serialize_color_component(builder, mode, m_properties.a, 125);
    builder.append(' ');
    serialize_color_component(builder, mode, m_properties.b, 125);
    if ((!m_properties.alpha->is_number() || m_properties.alpha->as_number().number() < 1)
        && (!m_properties.alpha->is_percentage() || m_properties.alpha->as_percentage().percentage().as_fraction() < 1)) {
        builder.append(" / "sv);
        serialize_alpha_component(builder, mode, m_properties.alpha);
    }

    builder.append(')');
}

}
