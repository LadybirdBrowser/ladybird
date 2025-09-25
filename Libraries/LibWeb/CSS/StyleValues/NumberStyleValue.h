/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class NumberStyleValue final : public StyleValue {
public:
    static ValueComparingNonnullRefPtr<NumberStyleValue const> create(double value)
    {
        return adopt_ref(*new (nothrow) NumberStyleValue(value));
    }

    double number() const { return m_value; }

    virtual String to_string(SerializationMode) const override;
    virtual Vector<Parser::ComponentValue> tokenize() const override;
    virtual GC::Ref<CSSStyleValue> reify(JS::Realm&, FlyString const& associated_property) const override;

    bool equals(StyleValue const& other) const override
    {
        if (type() != other.type())
            return false;
        auto const& other_number = other.as_number();
        return m_value == other_number.m_value;
    }

private:
    explicit NumberStyleValue(double value)
        : StyleValue(Type::Number)
        , m_value(value)
    {
    }

    double m_value { 0 };
};

}
