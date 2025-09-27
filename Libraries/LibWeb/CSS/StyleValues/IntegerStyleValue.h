/*
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class IntegerStyleValue final : public StyleValue {
public:
    static ValueComparingNonnullRefPtr<IntegerStyleValue const> create(i64 value)
    {
        return adopt_ref(*new (nothrow) IntegerStyleValue(value));
    }

    i64 integer() const { return m_value; }

    virtual String to_string(SerializationMode) const override;
    virtual Vector<Parser::ComponentValue> tokenize() const override;
    virtual GC::Ref<CSSStyleValue> reify(JS::Realm&, FlyString const& associated_property) const override;

    bool equals(StyleValue const& other) const override
    {
        if (type() != other.type())
            return false;
        auto const& other_integer = other.as_integer();
        return m_value == other_integer.m_value;
    }

private:
    explicit IntegerStyleValue(i64 value)
        : StyleValue(Type::Integer)
        , m_value(value)
    {
    }

    i64 m_value { 0 };
};

}
