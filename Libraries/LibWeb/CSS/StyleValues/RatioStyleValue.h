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

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    void serialize(StringBuilder&, SerializationMode) const;
    void serialize(Utf16StringBuilder&, SerializationMode) const;
    Vector<Parser::ComponentValue> tokenize() const;

    bool properties_equal(RatioStyleValue const& other) const
    {
        return numerator() == other.numerator()
            && denominator() == other.denominator();
    }

private:
    RatioStyleValue(ValueComparingNonnullRefPtr<StyleValue const> numerator, ValueComparingNonnullRefPtr<StyleValue const> denominator)
        : StyleValueWithDefaultOperators(Type::Ratio, StyleValueFFI::rust_style_value_create_ratio(&numerator.leak_ref(), &denominator.leak_ref()))
    {
    }

    ValueComparingNonnullRefPtr<StyleValue const> numerator() const { return *static_cast<StyleValue const*>(m_value->ratio.numerator.pointer); }
    ValueComparingNonnullRefPtr<StyleValue const> denominator() const { return *static_cast<StyleValue const*>(m_value->ratio.denominator.pointer); }
};

}
