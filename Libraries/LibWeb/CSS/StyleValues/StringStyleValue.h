/*
 * Copyright (c) 2022-2024, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/Parser/ComponentValue.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class StringStyleValue : public StyleValueWithDefaultOperators<StringStyleValue> {
public:
    static ValueComparingNonnullRefPtr<StringStyleValue const> create(Utf16FlyString string)
    {
        return adopt_ref(*new (nothrow) StringStyleValue(move(string)));
    }
    virtual ~StringStyleValue() override = default;

    Utf16FlyString string_value() const { return Utf16FlyString::from_raw(m_value->string.string.raw); }
    void serialize(StringBuilder& builder, SerializationMode) const { builder.append(serialize_a_string(string_value())); }
    Vector<Parser::ComponentValue> tokenize() const
    {
        return { Parser::Token::create_string(string_value()) };
    }

    bool properties_equal(StringStyleValue const& other) const { return string_value() == other.string_value(); }

private:
    explicit StringStyleValue(Utf16FlyString string)
        : StyleValueWithDefaultOperators(Type::String, StyleValueFFI::rust_style_value_create_string(string.to_raw_leaked()))
    {
    }
};

}
