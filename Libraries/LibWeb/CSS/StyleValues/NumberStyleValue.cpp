/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "NumberStyleValue.h"

namespace Web::CSS {

String NumberStyleValue::to_string(SerializationMode) const
{
    // FIXME: This should be moved into Serialize.cpp and used by applicable dimensions as well.
    // https://drafts.csswg.org/cssom/#serialize-a-css-value
    // <number>
    // A base-ten number using digits 0-9 (U+0030 to U+0039) in the shortest form possible, using "." to separate
    // decimals (if any), rounding the value if necessary to not produce more than 6 decimals, preceded by "-" (U+002D)
    // if it is negative.
    return MUST(String::formatted("{:.6}", m_value));
}

Vector<Parser::ComponentValue> NumberStyleValue::tokenize() const
{
    return { Parser::Token::create_number(Number { Number::Type::Number, m_value }) };
}

}
