/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "RatioStyleValue.h"
#include <LibWeb/CSS/Parser/ComponentValue.h>
#include <LibWeb/CSS/Ratio.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>

namespace Web::CSS {

Ratio RatioStyleValue::resolved() const
{
    return { number_from_style_value(m_numerator, {}), number_from_style_value(m_denominator, {}) };
}

ValueComparingNonnullRefPtr<StyleValue const> RatioStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto absolutized_numerator = m_numerator->absolutized(computation_context);
    auto absolutized_denominator = m_denominator->absolutized(computation_context);

    if (absolutized_numerator == m_numerator && absolutized_denominator == m_denominator)
        return *this;

    return RatioStyleValue::create(move(absolutized_numerator), move(absolutized_denominator));
}

void RatioStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    m_numerator->serialize(builder, mode);
    builder.append(" / "sv);
    m_denominator->serialize(builder, mode);
}

Vector<Parser::ComponentValue> RatioStyleValue::tokenize() const
{
    Vector<Parser::ComponentValue> component_values;

    component_values.extend(m_numerator->tokenize());
    component_values.empend(Parser::Token::create_whitespace(" "_string));
    component_values.empend(Parser::Token::create_delim('/'));
    component_values.empend(Parser::Token::create_whitespace(" "_string));
    component_values.extend(m_denominator->tokenize());

    return component_values;
}

}
