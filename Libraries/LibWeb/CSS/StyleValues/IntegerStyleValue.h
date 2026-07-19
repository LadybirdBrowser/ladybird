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
        // Small integers are common enough (z-index, column counts, spans) that they are
        // interned, making repeated creations allocation-free.
        static constexpr i32 first_interned_value = -1;
        static constexpr i32 last_interned_value = 255;
        if (value >= first_interned_value && value <= last_interned_value) {
            static auto const& instances = *[] {
                auto* instances = new (nothrow) Vector<NonnullRefPtr<IntegerStyleValue const>>();
                instances->ensure_capacity(last_interned_value - first_interned_value + 1);
                for (i32 interned_value = first_interned_value; interned_value <= last_interned_value; ++interned_value)
                    instances->unchecked_append(adopt_ref(*new (nothrow) IntegerStyleValue(interned_value)));
                return instances;
            }();
            return instances[value - first_interned_value];
        }
        return adopt_ref(*new (nothrow) IntegerStyleValue(value));
    }

    i32 integer() const { return m_value->integer.value; }

    void serialize(StringBuilder&, SerializationMode) const;
    void serialize(Utf16StringBuilder&, SerializationMode) const;
    Vector<Parser::ComponentValue> tokenize() const;
    GC::Ref<CSSStyleValue> reify(JS::Realm&, Utf16FlyString const& associated_property) const;

    bool equals(StyleValue const& other) const
    {
        if (type() != other.type())
            return false;
        auto const& other_integer = other.as_integer();
        return integer() == other_integer.integer();
    }

private:
    explicit IntegerStyleValue(i32 value)
        : StyleValue(Type::Integer, StyleValueFFI::rust_style_value_create_integer(value))
    {
    }
};

}
