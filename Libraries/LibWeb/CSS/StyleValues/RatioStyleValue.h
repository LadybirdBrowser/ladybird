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

private:
    friend class StyleValue;

    explicit RatioStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::Ratio, data)
    {
    }

    RatioStyleValue(ValueComparingNonnullRefPtr<StyleValue const> numerator, ValueComparingNonnullRefPtr<StyleValue const> denominator)
        : StyleValueWithDefaultOperators(Type::Ratio, StyleValueFFI::rust_style_value_create_ratio(StyleValueFFI::rust_style_value_retain(numerator->rust_style_value_data()), StyleValueFFI::rust_style_value_retain(denominator->rust_style_value_data())))
    {
    }

    ValueComparingNonnullRefPtr<StyleValue const> numerator() const { return wrap_rust_child(m_value->ratio.numerator); }
    ValueComparingNonnullRefPtr<StyleValue const> denominator() const { return wrap_rust_child(m_value->ratio.denominator); }
};

}
