/*
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

// https://www.w3.org/TR/css-values-4/#custom-idents
class CustomIdentStyleValue final : public StyleValueWithDefaultOperators<CustomIdentStyleValue> {
public:
    static ValueComparingNonnullRefPtr<CustomIdentStyleValue const> create(Utf16FlyString custom_ident)
    {
        return adopt_ref(*new (nothrow) CustomIdentStyleValue(move(custom_ident)));
    }
    virtual ~CustomIdentStyleValue() override = default;

    Utf16FlyString custom_ident() const { return Utf16FlyString::from_raw(m_value->custom_ident.custom_ident.raw); }

    void serialize(StringBuilder& builder, SerializationMode) const { builder.append(serialize_an_identifier(custom_ident())); }
    Vector<Parser::ComponentValue> tokenize() const;
    GC::Ref<CSSStyleValue> reify(JS::Realm& realm, Utf16FlyString const&) const;

    bool properties_equal(CustomIdentStyleValue const& other) const { return custom_ident() == other.custom_ident(); }

private:
    explicit CustomIdentStyleValue(Utf16FlyString custom_ident)
        : StyleValueWithDefaultOperators(Type::CustomIdent, StyleValueFFI::rust_style_value_create_custom_ident(custom_ident.to_raw_leaked()))
    {
    }
};

}
