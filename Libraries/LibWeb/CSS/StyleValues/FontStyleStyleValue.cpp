/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FontStyleStyleValue.h"
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>

namespace Web::CSS {

FontStyleStyleValue::FontStyleStyleValue(FontStyle font_style, ValueComparingRefPtr<CSSStyleValue const> angle_value)
    : StyleValueWithDefaultOperators(Type::FontStyle)
    , m_font_style(font_style)
    , m_angle_value(angle_value)
{
}

FontStyleStyleValue::~FontStyleStyleValue() = default;

String FontStyleStyleValue::to_string(SerializationMode mode) const
{
    Optional<String> angle_string;
    if (m_angle_value) {
        angle_string = m_angle_value->to_string(mode);
        if (m_font_style == FontStyle::Oblique && angle_string == "0deg"sv)
            return "normal"_string;
    }
    StringBuilder builder;
    builder.append(CSS::to_string(m_font_style));
    // https://drafts.csswg.org/css-fonts/#valdef-font-style-oblique-angle--90deg-90deg
    // The lack of an <angle> represents 14deg. (Note that a font might internally provide its own mapping for "oblique", but the mapping within the font is disregarded.)
    static auto default_angle = Angle::make_degrees(14);
    if (angle_string.has_value() && !(m_angle_value->is_angle() && m_angle_value->as_angle().angle() == default_angle))
        builder.appendff(" {}", angle_string);

    return MUST(builder.to_string());
}

}
