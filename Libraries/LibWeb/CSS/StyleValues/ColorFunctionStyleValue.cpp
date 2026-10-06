/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ColorFunctionStyleValue.h"
#include <AK/Math.h>
#include <LibGfx/ColorConversion.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>

namespace Web::CSS {

ColorFunctionStyleValue::ColorFunctionStyleValue(StyleValueFFI::StyleValueData const* data)
    : ColorStyleValue(data)
{
}

ValueComparingNonnullRefPtr<ColorFunctionStyleValue const> ColorFunctionStyleValue::create(
    ColorType color_type,
    ValueComparingNonnullRefPtr<StyleValue const> c1,
    ValueComparingNonnullRefPtr<StyleValue const> c2,
    ValueComparingNonnullRefPtr<StyleValue const> c3,
    ValueComparingRefPtr<StyleValue const> alpha,
    ColorSyntax color_syntax,
    Optional<Utf16FlyString> name,
    ValueComparingRefPtr<StyleValue const> origin_color)
{
    auto const& descriptor = color_function_descriptor_for(color_type);
    VERIFY(descriptor.serialization_behavior == SerializationBehavior::SrgbLegacy || color_syntax == ColorSyntax::Modern);

    return adopt_ref(*new (nothrow) ColorFunctionStyleValue(
        color_type, move(c1), move(c2), move(c3), move(alpha), color_syntax, move(name), move(origin_color)));
}

}
