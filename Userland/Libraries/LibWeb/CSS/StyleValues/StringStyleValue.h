/*
 * Copyright (c) 2022-2024, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

class StringStyleValue : public StyleValueWithDefaultOperators<StringStyleValue> {
public:
    static ValueComparingNonnullRefPtr<StringStyleValue> create(FlyString const& string)
    {
        return adopt_ref(*new (nothrow) StringStyleValue(string));
    }
    virtual ~StringStyleValue() override = default;

    FlyString const& string_value() const { return m_string; }
    String to_string() const override { return serialize_a_string(m_string); }

    bool properties_equal(StringStyleValue const& other) const { return m_string == other.m_string; }

private:
    explicit StringStyleValue(FlyString const& string)
        : StyleValueWithDefaultOperators(Type::String)
        , m_string(string)
    {
    }

    FlyString m_string;
};

}
