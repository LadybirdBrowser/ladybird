/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/Ratio.h>

namespace Web::CSS {

class RatioStyleValue final : public StyleValueWithDefaultOperators<RatioStyleValue> {
public:
    static ValueComparingNonnullRefPtr<RatioStyleValue> create(Ratio ratio)
    {
        return adopt_ref(*new (nothrow) RatioStyleValue(move(ratio)));
    }
    virtual ~RatioStyleValue() override = default;

    Ratio const& ratio() const { return m_ratio; }
    Ratio& ratio() { return m_ratio; }

    virtual String to_string() const override { return m_ratio.to_string(); }

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
