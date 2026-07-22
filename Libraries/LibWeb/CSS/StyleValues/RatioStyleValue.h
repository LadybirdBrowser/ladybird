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
    friend class StyleValue;

    explicit RatioStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::Ratio, data)
        , m_numerator(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(data->ratio.numerator.pointer))))
        , m_denominator(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(data->ratio.denominator.pointer))))
    {
    }

    RatioStyleValue(ValueComparingNonnullRefPtr<StyleValue const> numerator, ValueComparingNonnullRefPtr<StyleValue const> denominator)
        : StyleValueWithDefaultOperators(Type::Ratio, StyleValueFFI::rust_style_value_create_ratio(StyleValueFFI::rust_style_value_retain(numerator->rust_style_value_data()), StyleValueFFI::rust_style_value_retain(denominator->rust_style_value_data())))
        , m_numerator(move(numerator))
        , m_denominator(move(denominator))
    {
    }

    ValueComparingNonnullRefPtr<StyleValue const> numerator() const { return m_numerator; }
    ValueComparingNonnullRefPtr<StyleValue const> denominator() const { return m_denominator; }

    ValueComparingNonnullRefPtr<StyleValue const> m_numerator;
    ValueComparingNonnullRefPtr<StyleValue const> m_denominator;
};

}
