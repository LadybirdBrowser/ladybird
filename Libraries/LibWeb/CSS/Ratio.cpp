/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Ratio.h"
#include <LibWeb/CSS/Serialize.h>
#include <math.h>

namespace Web::CSS {

Ratio::Ratio(double first, double second)
    : m_first_value(first)
    , m_second_value(second)
{
}

// https://www.w3.org/TR/css-values-4/#degenerate-ratio
bool Ratio::is_degenerate() const
{
    return !isfinite(m_first_value) || m_first_value == 0
        || !isfinite(m_second_value) || m_second_value == 0;
}

String Ratio::to_string() const
{
    // https://drafts.csswg.org/cssom/#serialize-a-css-value
    // -> <ratio>
    // The numerator serialized as per <number> followed by the literal string " / ", followed by the denominator
    // serialized as per <number>.
    StringBuilder builder;
    serialize_a_number(builder, m_first_value);
    builder.append(" / "sv);
    serialize_a_number(builder, m_second_value);
    return builder.to_string_without_validation();
}

}
