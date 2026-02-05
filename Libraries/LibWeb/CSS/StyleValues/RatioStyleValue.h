/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Ratio.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class RatioStyleValue final : public StyleValueWithDefaultOperators<RatioStyleValue> {
public:
    static ValueComparingNonnullRefPtr<RatioStyleValue const> create(Ratio ratio)
    {
        return adopt_ref(*new (nothrow) RatioStyleValue(move(ratio)));
    }
    virtual ~RatioStyleValue() override = default;

    Ratio const& ratio() const { return m_ratio; }
    Ratio& ratio() { return m_ratio; }

    virtual void serialize(StringBuilder& builder, SerializationMode) const override { builder.append(m_ratio.to_string()); }
    Vector<Parser::ComponentValue> tokenize() const override
    {
        return {
            Parser::Token::create_number(Number { Number::Type::Number, m_ratio.numerator() }),
            Parser::Token::create_whitespace(" "_string),
            Parser::Token::create_delim('/'),
            Parser::Token::create_whitespace(" "_string),
            Parser::Token::create_number(Number { Number::Type::Number, m_ratio.denominator() }),
        };
    }

    bool properties_equal(RatioStyleValue const& other) const { return m_ratio == other.m_ratio; }

private:
    RatioStyleValue(Ratio&& ratio)
        : StyleValueWithDefaultOperators(Type::Ratio)
        , m_ratio(ratio)
    {
    }

    Ratio m_ratio;
};

}
