/*
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/RustStyleValueHandle.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class IntegerStyleValue final : public StyleValue {
public:
    static ValueComparingNonnullRefPtr<IntegerStyleValue const> create(i32 value)
    {
        return adopt_ref(*new (nothrow) IntegerStyleValue(value));
    }

    i32 integer() const { return m_value->integer.value; }

    virtual void serialize(StringBuilder&, SerializationMode) const override;
    virtual void serialize(Utf16StringBuilder&, SerializationMode) const override;
    virtual Vector<Parser::ComponentValue> tokenize() const override;
    virtual GC::Ref<CSSStyleValue> reify(JS::Realm&, Utf16FlyString const& associated_property) const override;

    bool equals(StyleValue const& other) const override
    {
        if (type() != other.type())
            return false;
        auto const& other_integer = other.as_integer();
        return integer() == other_integer.integer();
    }

    virtual bool is_computationally_independent() const override { return true; }

private:
    explicit IntegerStyleValue(i32 value)
        : StyleValue(Type::Integer)
        , m_value(StyleValueFFI::rust_style_value_create_integer(value))
    {
    }

    RustStyleValueHandle m_value;
};

}
