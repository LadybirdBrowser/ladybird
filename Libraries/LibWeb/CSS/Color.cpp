/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Color.h"
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

Color::Color(Gfx::Color resolved_color, ValueComparingRefPtr<StyleValue const> style_value)
    : m_style_value(move(style_value))
    , m_srgba(resolved_color.red() / 255.0f, resolved_color.green() / 255.0f, resolved_color.blue() / 255.0f, resolved_color.alpha() / 255.0f)
{
}

void Color::serialize(StringBuilder& builder, SerializationMode serialization_mode) const
{
    if (m_style_value) {
        m_style_value->serialize(builder, serialization_mode);
    } else {
        resolved().serialize_a_srgb_value(builder);
    }
}

String Color::to_string(SerializationMode serialization_mode) const
{
    StringBuilder builder;
    serialize(builder, serialization_mode);
    return builder.to_string_without_validation();
}

Gfx::Color Color::resolved() const
{
    auto clamp_255 = [](float input) -> u8 {
        return clamp(lroundf(input * 255), 0, 255);
    };
    return Gfx::Color {
        clamp_255(m_srgba.red),
        clamp_255(m_srgba.green),
        clamp_255(m_srgba.blue),
        clamp_255(m_srgba.alpha),
    };
}

}
