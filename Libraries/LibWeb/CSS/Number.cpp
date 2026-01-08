/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/CSS/Number.h>
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

void Number::serialize(StringBuilder& builder, SerializationMode) const
{
    if (m_type == Type::IntegerWithExplicitSign) {
        builder.appendff("{:+}", m_value);
        return;
    }
    if (m_value == AK::Infinity<double>) {
        builder.append("infinity"sv);
        return;
    }
    if (m_value == -AK::Infinity<double>) {
        builder.append("-infinity"sv);
        return;
    }
    if (isnan(m_value)) {
        builder.append("NaN"sv);
        return;
    }
    serialize_a_number(builder, m_value);
}

String Number::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    serialize(builder, mode);
    return builder.to_string_without_validation();
}

}
