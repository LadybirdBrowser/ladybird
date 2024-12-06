/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibWeb/CSS/StyleValues/ScaleStyleValue.h>

namespace Web::CSS {

// https://www.w3.org/TR/2021/WD-css-transforms-2-20211109/#individual-transform-serialization
String ScaleStyleValue::to_string(SerializationMode) const
{
    auto resolve_to_string = [](NumberPercentage const& value) -> String {
        if (value.is_number()) {
            return MUST(String::formatted("{}", value.number().value()));
        }
        if (value.is_percentage()) {
            return MUST(String::formatted("{}", value.percentage().value() / 100.0));
        }
        return value.to_string();
    };

    auto x_value = resolve_to_string(m_properties.x);
    auto y_value = resolve_to_string(m_properties.y);

    StringBuilder builder;
    builder.append(x_value);
    if (x_value != y_value) {
        builder.append(" "sv);
        builder.append(y_value);
    }
    return builder.to_string_without_validation();
}

}
