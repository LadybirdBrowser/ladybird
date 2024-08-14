/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/CSSUnitValue.h>

namespace Web::CSS {

class NumberStyleValue final : public CSSUnitValue {
public:
    static ValueComparingNonnullRefPtr<NumberStyleValue> create(double value)
    {
        return adopt_ref(*new (nothrow) NumberStyleValue(value));
    }

    double number() const { return m_value; }
    virtual double value() const override { return m_value; }
    virtual StringView unit() const override { return "number"sv; }

    virtual String to_string() const override;

    bool equals(CSSStyleValue const& other) const override
    {
        if (type() != other.type())
            return false;
        auto const& other_number = other.as_number();
        return m_value == other_number.m_value;
    }

private:
    explicit NumberStyleValue(double value)
        : CSSUnitValue(Type::Number)
        , m_value(value)
    {
    }

    double m_value { 0 };
};

}
