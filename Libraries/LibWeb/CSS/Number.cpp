/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/CSS/Number.h>

namespace Web::CSS {

String Number::to_string() const
{
    if (m_type == Type::IntegerWithExplicitSign)
        return MUST(String::formatted("{:+}", m_value));
    if (m_value == AK::Infinity<double>)
        return "infinity"_string;
    if (m_value == -AK::Infinity<double>)
        return "-infinity"_string;
    if (isnan(m_value))
        return "NaN"_string;
    return String::number(m_value);
}

}
