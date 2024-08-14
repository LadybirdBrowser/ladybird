/*
 * Copyright (c) 2023-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/CSSUnitValue.h>

namespace Web::CSS {

class IntegerStyleValue final : public CSSUnitValue {
public:
    static ValueComparingNonnullRefPtr<IntegerStyleValue> create(i64 value)
    {
        return adopt_ref(*new (nothrow) IntegerStyleValue(value));
    }

    i64 integer() const { return m_value; }
    virtual double value() const override { return m_value; }
    virtual StringView unit() const override { return "number"sv; }

    virtual String to_string() const override;

    bool equals(CSSStyleValue const& other) const override
    {
        if (type() != other.type())
            return false;
        auto const& other_integer = other.as_integer();
        return m_value == other_integer.m_value;
    }

private:
    explicit IntegerStyleValue(i64 value)
        : CSSUnitValue(Type::Integer)
        , m_value(value)
    {
    }

    i64 m_value { 0 };
};

}
