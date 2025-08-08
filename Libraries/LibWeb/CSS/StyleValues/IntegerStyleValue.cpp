/*
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "IntegerStyleValue.h"
#include <LibWeb/CSS/Parser/ComponentValue.h>

namespace Web::CSS {

String IntegerStyleValue::to_string(SerializationMode) const
{
    return String::number(m_value);
}

Vector<Parser::ComponentValue> IntegerStyleValue::tokenize() const
{
    return { Parser::Token::create_number(Number { Number::Type::Integer, static_cast<double>(m_value) }) };
}

}
