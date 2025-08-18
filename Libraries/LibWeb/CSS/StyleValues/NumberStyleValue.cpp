/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "NumberStyleValue.h"
#include <LibWeb/CSS/Parser/ComponentValue.h>
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

String NumberStyleValue::to_string(SerializationMode) const
{
    return serialize_a_number(m_value);
}

Vector<Parser::ComponentValue> NumberStyleValue::tokenize() const
{
    return { Parser::Token::create_number(Number { Number::Type::Number, m_value }) };
}

}
