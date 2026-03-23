/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class RatioStyleValue final : public StyleValueWithDefaultOperators<RatioStyleValue> {
public:
    static ValueComparingNonnullRefPtr<RatioStyleValue const> create(ValueComparingNonnullRefPtr<StyleValue const> numerator, ValueComparingNonnullRefPtr<StyleValue const> denominator)
    {
        return adopt_ref(*new (nothrow) RatioStyleValue(move(numerator), move(denominator)));
    }
    virtual ~RatioStyleValue() override = default;

    Ratio resolved() const;

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    virtual void serialize(StringBuilder&, SerializationMode) const override;
    Vector<Parser::ComponentValue> tokenize() const override;

    bool properties_equal(RatioStyleValue const& other) const
    {
        return m_numerator == other.m_numerator
            && m_denominator == other.m_denominator;
    }

private:
    RatioStyleValue(ValueComparingNonnullRefPtr<StyleValue const> numerator, ValueComparingNonnullRefPtr<StyleValue const> denominator)
        : StyleValueWithDefaultOperators(Type::Ratio)
        , m_numerator(move(numerator))
        , m_denominator(move(denominator))
    {
    }

    ValueComparingNonnullRefPtr<StyleValue const> m_numerator;
    ValueComparingNonnullRefPtr<StyleValue const> m_denominator;
};

}
