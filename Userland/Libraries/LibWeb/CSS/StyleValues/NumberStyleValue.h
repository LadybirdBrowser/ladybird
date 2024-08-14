/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

class NumberStyleValue : public StyleValueWithDefaultOperators<NumberStyleValue> {
public:
    static ValueComparingNonnullRefPtr<NumberStyleValue> create(double value)
    {
        return adopt_ref(*new (nothrow) NumberStyleValue(value));
    }

    double number() const { return m_value; }

    virtual String to_string() const override;

    bool properties_equal(NumberStyleValue const& other) const { return m_value == other.m_value; }

private:
    explicit NumberStyleValue(double value)
        : StyleValueWithDefaultOperators(Type::Number)
        , m_value(value)
    {
    }

    double m_value { 0 };
};

}
