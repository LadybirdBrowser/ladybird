/*
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "IntegerStyleValue.h"

namespace Web::CSS {

String IntegerStyleValue::to_string(SerializationMode) const
{
    return String::number(m_value);
}

Vector<Parser::ComponentValue> IntegerStyleValue::tokenize() const
{
    return { Parser::Token::create_number(Number { Number::Type::Integer, value() }) };
}

}
