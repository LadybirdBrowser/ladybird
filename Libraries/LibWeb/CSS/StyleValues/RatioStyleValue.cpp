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
    return { number_from_style_value(numerator(), {}), number_from_style_value(denominator(), {}) };
}

ValueComparingNonnullRefPtr<StyleValue const> RatioStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto absolutized_numerator = numerator()->absolutized(computation_context);
    auto absolutized_denominator = denominator()->absolutized(computation_context);

    if (absolutized_numerator == numerator() && absolutized_denominator == denominator())
        return *this;

    return RatioStyleValue::create(move(absolutized_numerator), move(absolutized_denominator));
}

Vector<Parser::ComponentValue> RatioStyleValue::tokenize() const
{
    Vector<Parser::ComponentValue> component_values;

    component_values.extend(numerator()->tokenize());
    component_values.empend(Parser::Token::create_whitespace(" "_utf16));
    component_values.empend(Parser::Token::create_delim('/'));
    component_values.empend(Parser::Token::create_whitespace(" "_utf16));
    component_values.extend(denominator()->tokenize());

    return component_values;
}

}
