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
    : StyleValueWithDefaultOperators(Type::FontStyle, make_font_style_data(font_style, angle_value))
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

ValueComparingNonnullRefPtr<StyleValue const> FontStyleStyleValue::absolutized(ComputationContext const& computation_context) const
{
    ValueComparingRefPtr<StyleValue const> absolutized_angle;

    if (angle())
        absolutized_angle = angle()->absolutized(computation_context);

    if (absolutized_angle == angle())
        return *this;

    return FontStyleStyleValue::create(font_style(), absolutized_angle);
}

}
