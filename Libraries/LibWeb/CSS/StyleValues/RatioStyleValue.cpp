/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "RatioStyleValue.h"
#include <LibWeb/CSS/Parser/ComponentValue.h>

namespace Web::CSS {

void RatioStyleValue::serialize(StringBuilder& builder, SerializationMode) const
{
    builder.append(m_ratio.to_string());
}

Vector<Parser::ComponentValue> RatioStyleValue::tokenize() const
{
    return {
        Parser::Token::create_number(Number { Number::Type::Number, m_ratio.numerator() }),
        Parser::Token::create_whitespace(" "_string),
        Parser::Token::create_delim('/'),
        Parser::Token::create_whitespace(" "_string),
        Parser::Token::create_number(Number { Number::Type::Number, m_ratio.denominator() }),
    };
}

}
