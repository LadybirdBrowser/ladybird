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
#include <LibWeb/Export.h>

namespace Web::CSS {

class WEB_API NumberStyleValue final : public StyleValue {
public:
    static ValueComparingNonnullRefPtr<NumberStyleValue const> create(double value)
    {
        // Zero and one dominate real-world number values (opacity, alpha, flex factors), so
        // those two are interned.
        if (value == 0 && !signbit(value)) {
            static auto const& zero_instance = adopt_ref(*new (nothrow) NumberStyleValue(0)).leak_ref();
            return zero_instance;
        }
        if (value == 1) {
            static auto const& one_instance = adopt_ref(*new (nothrow) NumberStyleValue(1)).leak_ref();
            return one_instance;
        }
        return adopt_ref(*new (nothrow) NumberStyleValue(value));
    }

    double number() const { return m_value->number.value; }

    void serialize(StringBuilder&, SerializationMode) const;
    Vector<Parser::ComponentValue> tokenize() const;
    GC::Ref<CSSStyleValue> reify(JS::Realm&, Utf16FlyString const& associated_property) const;

    bool equals(StyleValue const& other) const
    {
        if (type() != other.type())
            return false;
        auto const& other_number = other.as_number();
        return number() == other_number.number();
    }

private:
    explicit NumberStyleValue(double value)
        : StyleValue(Type::Number, StyleValueFFI::rust_style_value_create_number(value))
    {
    }
};

}
