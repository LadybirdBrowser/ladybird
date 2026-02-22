/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FontStyleStyleValue.h"
#include <LibGfx/Font/FontStyleMapping.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

FontStyleStyleValue::FontStyleStyleValue(FontStyleKeyword font_style, ValueComparingRefPtr<StyleValue const> angle_value)
    : StyleValueWithDefaultOperators(Type::FontStyle)
    , m_font_style(font_style)
    , m_angle_value(angle_value)
{
}

FontStyleStyleValue::~FontStyleStyleValue() = default;

int FontStyleStyleValue::to_font_slope() const
{
    // FIXME: Implement `left`, `right`, and `oblique <angle>`
    switch (as_font_style().font_style()) {
    case FontStyleKeyword::Italic:
        static int italic_slope = Gfx::name_to_slope("Italic"sv);
        return italic_slope;
    case FontStyleKeyword::Oblique:
        static int oblique_slope = Gfx::name_to_slope("Oblique"sv);
        return oblique_slope;
    case FontStyleKeyword::Normal:
    default:
        static int normal_slope = Gfx::name_to_slope("Normal"sv);
        return normal_slope;
    }
}

void FontStyleStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    Optional<String> angle_string;
    if (m_angle_value) {
        angle_string = m_angle_value->to_string(mode);
        if (m_font_style == FontStyleKeyword::Oblique && angle_string == "0deg"sv) {
            builder.append("normal"sv);
            return;
        }
    }
    builder.append(CSS::to_string(m_font_style));
    // https://drafts.csswg.org/css-fonts/#valdef-font-style-oblique-angle--90deg-90deg
    // The lack of an <angle> represents 14deg. (Note that a font might internally provide its own mapping for "oblique", but the mapping within the font is disregarded.)
    if (angle_string.has_value() && angle_string != "14deg"sv)
        builder.appendff(" {}", angle_string);
}

ValueComparingNonnullRefPtr<StyleValue const> FontStyleStyleValue::absolutized(ComputationContext const& computation_context) const
{
    ValueComparingRefPtr<StyleValue const> absolutized_angle;

    if (m_angle_value)
        absolutized_angle = m_angle_value->absolutized(computation_context);

    if (absolutized_angle == m_angle_value)
        return *this;

    return FontStyleStyleValue::create(m_font_style, absolutized_angle);
}

}
