/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TransformationStyleValue.h"
#include <AK/StringBuilder.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>

namespace Web::CSS {

String TransformationStyleValue::to_string() const
{
    StringBuilder builder;
    builder.append(CSS::to_string(m_properties.transform_function));
    builder.append('(');
    for (size_t i = 0; i < m_properties.values.size(); ++i) {
        auto const& value = m_properties.values[i];

        // https://www.w3.org/TR/css-transforms-2/#individual-transforms
        // A <percentage> is equivalent to a <number>, for example scale: 100% is equivalent to scale: 1.
        // Numbers are used during serialization of specified and computed values.
        if ((m_properties.transform_function == CSS::TransformFunction::Scale
                || m_properties.transform_function == CSS::TransformFunction::Scale3d
                || m_properties.transform_function == CSS::TransformFunction::ScaleX
                || m_properties.transform_function == CSS::TransformFunction::ScaleY
                || m_properties.transform_function == CSS::TransformFunction::ScaleZ)
            && value->is_percentage()) {
            builder.append(MUST(String::number(value->as_percentage().percentage().as_fraction())));
        } else {
            builder.append(value->to_string());
        }

        if (i != m_properties.values.size() - 1)
            builder.append(", "sv);
    }
    builder.append(')');

    return MUST(builder.to_string());
}

bool TransformationStyleValue::Properties::operator==(Properties const& other) const
{
    return transform_function == other.transform_function && values.span() == other.values.span();
}

}
