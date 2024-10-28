/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSLCHLike.h"
#include <AK/Math.h>
#include <AK/TypeCasts.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CSSMathValue.h>
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
    auto const& other_oklch_like = verify_cast<CSSLCHLike>(other_color);
    return m_properties == other_oklch_like.m_properties;
}

Color CSSLCH::to_color(Optional<Layout::NodeWithStyle const&>) const
{
    auto const l_val = clamp(resolve_with_reference_value(m_properties.l, 100).value_or(0), 0, 100);
    auto const c_val = resolve_with_reference_value(m_properties.c, 150).value_or(0);
    auto const h_val = AK::to_radians(resolve_hue(m_properties.h).value_or(0));
    auto const alpha_val = resolve_alpha(m_properties.alpha).value_or(1);

    return Color::from_lab(l_val, c_val * cos(h_val), c_val * sin(h_val), alpha_val);
}

// https://www.w3.org/TR/css-color-4/#serializing-lab-lch
String CSSLCH::to_string() const
{
    // FIXME: Do this properly, taking unresolved calculated values into account.
    return serialize_a_srgb_value(to_color({}));
}

Color CSSOKLCH::to_color(Optional<Layout::NodeWithStyle const&>) const
{
    auto const l_val = clamp(resolve_with_reference_value(m_properties.l, 1.0).value_or(0), 0, 1);
    auto const c_val = max(resolve_with_reference_value(m_properties.c, 0.4).value_or(0), 0);
    auto const h_val = AK::to_radians(resolve_hue(m_properties.h).value_or(0));
    auto const alpha_val = resolve_alpha(m_properties.alpha).value_or(1);

    return Color::from_oklab(l_val, c_val * cos(h_val), c_val * sin(h_val), alpha_val);
}

// https://www.w3.org/TR/css-color-4/#serializing-oklab-oklch
String CSSOKLCH::to_string() const
{
    // FIXME: Do this properly, taking unresolved calculated values into account.
    return serialize_a_srgb_value(to_color({}));
}

}
