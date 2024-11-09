/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSHSL.h"
#include <AK/TypeCasts.h>
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

Color CSSHSL::to_color(Optional<Layout::NodeWithStyle const&>) const
{
    auto const h_val = resolve_hue(m_properties.h).value_or(0);
    auto const s_val = resolve_with_reference_value(m_properties.s, 100.0).value_or(0);
    auto const l_val = resolve_with_reference_value(m_properties.l, 100.0).value_or(0);
    auto const alpha_val = resolve_alpha(m_properties.alpha).value_or(1);

    return Color::from_hsla(h_val, s_val / 100.0f, l_val / 100.0f, alpha_val);
}

bool CSSHSL::equals(CSSStyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_color = other.as_color();
    if (color_type() != other_color.color_type())
        return false;
    auto const& other_hsl = verify_cast<CSSHSL>(other_color);
    return m_properties == other_hsl.m_properties;
}

// https://www.w3.org/TR/css-color-4/#serializing-sRGB-values
String CSSHSL::to_string() const
{
    // FIXME: Do this properly, taking unresolved calculated values into account.
    return serialize_a_srgb_value(to_color({}));
}

}
