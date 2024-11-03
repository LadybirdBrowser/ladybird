/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSLabLike.h"
#include <AK/TypeCasts.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CSSMathValue.h>
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
    auto const& other_lab_like = verify_cast<CSSLabLike>(other_color);
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
String CSSOKLab::to_string() const
{
    // FIXME: Do this properly, taking unresolved calculated values into account.
    return serialize_a_srgb_value(to_color({}));
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
String CSSLab::to_string() const
{
    // FIXME: Do this properly, taking unresolved calculated values into account.
    return serialize_a_srgb_value(to_color({}));
}

}
