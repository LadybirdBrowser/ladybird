/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSHWB.h"
#include <AK/TypeCasts.h>
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

Color CSSHWB::to_color(Optional<Layout::NodeWithStyle const&>) const
{
    auto const h_val = resolve_hue(m_properties.h).value_or(0);
    auto const w_val = clamp(resolve_with_reference_value(m_properties.w, 100.0).value_or(0), 0, 100) / 100.0f;
    auto const b_val = clamp(resolve_with_reference_value(m_properties.b, 100.0).value_or(0), 0, 100) / 100.0f;
    auto const alpha_val = resolve_alpha(m_properties.alpha).value_or(1);

    if (w_val + b_val >= 1.0f) {
        auto to_byte = [](float value) {
            return round_to<u8>(clamp(value * 255.0f, 0.0f, 255.0f));
        };
        u8 gray = to_byte(w_val / (w_val + b_val));
        return Color(gray, gray, gray, to_byte(alpha_val));
    }

    float value = 1 - b_val;
    float saturation = 1 - (w_val / value);
    return Color::from_hsv(h_val, saturation, value).with_opacity(alpha_val);
}

bool CSSHWB::equals(CSSStyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_color = other.as_color();
    if (color_type() != other_color.color_type())
        return false;
    auto const& other_hwb = verify_cast<CSSHWB>(other_color);
    return m_properties == other_hwb.m_properties;
}

// https://www.w3.org/TR/css-color-4/#serializing-sRGB-values
String CSSHWB::to_string() const
{
    // FIXME: Do this properly, taking unresolved calculated values into account.
    return serialize_a_srgb_value(to_color({}));
}

}
