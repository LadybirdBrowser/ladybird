/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibWeb/CSS/StyleValues/CSSMathValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/TranslationStyleValue.h>

namespace Web::CSS {

// https://www.w3.org/TR/2021/WD-css-transforms-2-20211109/#individual-transform-serialization
String TranslationStyleValue::to_string() const
{
    auto resolve_to_string = [](LengthPercentage const& value) -> Optional<String> {
        if (value.is_length()) {
            if (value.length().raw_value() == 0)
                return {};
        }
        if (value.is_percentage()) {
            if (value.percentage().value() == 0)
                return {};
        }
        return value.to_string();
    };

    auto x_value = resolve_to_string(m_properties.x);
    auto y_value = resolve_to_string(m_properties.y);

    StringBuilder builder;
    builder.append(x_value.value_or("0px"_string));
    if (y_value.has_value()) {
        builder.append(" "sv);
        builder.append(y_value.value());
    }

    return builder.to_string_without_validation();
}

}
