/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/CSS/Number.h>
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

String Number::to_string(SerializationMode) const
{
    if (m_type == Type::IntegerWithExplicitSign)
        return MUST(String::formatted("{:+}", m_value));
    if (m_value == AK::Infinity<double>)
        return "infinity"_string;
    if (m_value == -AK::Infinity<double>)
        return "-infinity"_string;
    if (isnan(m_value))
        return "NaN"_string;
    return serialize_a_number(m_value);
}

}
