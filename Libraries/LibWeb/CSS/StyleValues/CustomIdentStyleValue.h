/*
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/Parser/ComponentValue.h>

namespace Web::CSS {

// https://www.w3.org/TR/css-values-4/#custom-idents
class CustomIdentStyleValue final : public StyleValueWithDefaultOperators<CustomIdentStyleValue> {
public:
    static ValueComparingNonnullRefPtr<CustomIdentStyleValue const> create(FlyString custom_ident)
    {
        return adopt_ref(*new (nothrow) CustomIdentStyleValue(move(custom_ident)));
    }
    virtual ~CustomIdentStyleValue() override = default;

    FlyString const& custom_ident() const { return m_custom_ident; }

    virtual String to_string(SerializationMode) const override { return m_custom_ident.to_string(); }
    virtual Vector<Parser::ComponentValue> tokenize() const override
    {
        return { Parser::Token::create_ident(m_custom_ident) };
    }

    bool properties_equal(CustomIdentStyleValue const& other) const { return m_custom_ident == other.m_custom_ident; }

private:
    explicit CustomIdentStyleValue(FlyString custom_ident)
        : StyleValueWithDefaultOperators(Type::CustomIdent)
        , m_custom_ident(move(custom_ident))
    {
    }

    FlyString m_custom_ident;
};

}
