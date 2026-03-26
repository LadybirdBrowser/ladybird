/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/CSS/Number.h>
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

i32 round_to_nearest_integer(double value)
{
    // https://drafts.csswg.org/css-values-4/#css-round-to-the-nearest-integer
    // Unless otherwise specified, in the CSS specifications rounding to the nearest integer requires rounding in
    // the direction of +∞ when the fractional portion is exactly 0.5.
    if (isnan(value))
        return 0;

    if (isinf(value)) {
        if (value > 0)
            return AK::NumericLimits<i32>::max();

        return AK::NumericLimits<i32>::min();
    }

    return AK::clamp_to<i32>(floor(value + 0.5));
}

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
