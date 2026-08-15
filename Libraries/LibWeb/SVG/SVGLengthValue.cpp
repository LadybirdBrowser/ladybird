/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16StringBuilder.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/SVG/SVGLengthValue.h>

namespace Web::SVG {

Optional<SVGLengthValue> SVGLengthValue::from_style_value(CSS::StyleValue const& style_value)
{
    if (style_value.is_number())
        return number(style_value.as_number().number());

    if (style_value.is_percentage())
        return percentage(style_value.as_percentage().percentage().value());

    if (style_value.is_length()) {
        auto const& css_length = style_value.as_length().length();
        return length(css_length.raw_value(), css_length.unit());
    }

    return {};
}

Utf16String SVGLengthValue::to_utf16_string() const
{
    switch (m_kind) {
    case Kind::Number: {
        Utf16StringBuilder builder;
        CSS::serialize_a_number(builder, m_value);
        return builder.to_string();
    }
    case Kind::Length: {
        Utf16StringBuilder builder;
        to_length().serialize(builder);
        return builder.to_string();
    }
    case Kind::Percentage:
        return to_percentage().to_utf16_string();
    }

    VERIFY_NOT_REACHED();
}

}
