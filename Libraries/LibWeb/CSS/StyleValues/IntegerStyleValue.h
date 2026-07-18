/*
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

class WEB_API IntegerStyleValue final : public StyleValue {
public:
    static ValueComparingNonnullRefPtr<IntegerStyleValue const> create(i32 value)
    {
        return adopt_ref(*new (nothrow) IntegerStyleValue(value));
    }

    i32 integer() const { return m_value->integer.value; }

    virtual void serialize(StringBuilder&, SerializationMode) const override;
    void serialize(Utf16StringBuilder&, SerializationMode) const;
    Vector<Parser::ComponentValue> tokenize() const;
    GC::Ref<CSSStyleValue> reify(JS::Realm&, Utf16FlyString const& associated_property) const;

    bool equals(StyleValue const& other) const override
    {
        if (type() != other.type())
            return false;
        auto const& other_integer = other.as_integer();
        return integer() == other_integer.integer();
    }

    bool is_computationally_independent() const { return true; }

private:
    explicit IntegerStyleValue(i32 value)
        : StyleValue(Type::Integer, StyleValueFFI::rust_style_value_create_integer(value))
    {
    }
};

}
