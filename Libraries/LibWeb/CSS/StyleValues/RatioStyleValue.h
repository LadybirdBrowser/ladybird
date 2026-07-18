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
    void serialize(Utf16StringBuilder&, SerializationMode) const;
    Vector<Parser::ComponentValue> tokenize() const;

    bool properties_equal(RatioStyleValue const& other) const
    {
        return numerator() == other.numerator()
            && denominator() == other.denominator();
    }

    bool is_computationally_independent() const { return numerator()->is_computationally_independent() && denominator()->is_computationally_independent(); }

private:
    RatioStyleValue(ValueComparingNonnullRefPtr<StyleValue const> numerator, ValueComparingNonnullRefPtr<StyleValue const> denominator)
        : StyleValueWithDefaultOperators(Type::Ratio, StyleValueFFI::rust_style_value_create_ratio(&numerator.leak_ref(), &denominator.leak_ref()))
    {
    }

    ValueComparingNonnullRefPtr<StyleValue const> numerator() const { return *static_cast<StyleValue const*>(m_value->ratio.numerator.pointer); }
    ValueComparingNonnullRefPtr<StyleValue const> denominator() const { return *static_cast<StyleValue const*>(m_value->ratio.denominator.pointer); }
};

}
