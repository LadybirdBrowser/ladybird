/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

// https://www.w3.org/TR/css-values-4/#custom-idents
class CustomIdentStyleValue final : public StyleValueWithDefaultOperators<CustomIdentStyleValue> {
public:
    static ValueComparingNonnullRefPtr<CustomIdentStyleValue> create(FlyString custom_ident)
    {
        return adopt_ref(*new (nothrow) CustomIdentStyleValue(move(custom_ident)));
    }
    virtual ~CustomIdentStyleValue() override = default;

    FlyString const& custom_ident() const { return m_custom_ident; }

    virtual String to_string() const override { return m_custom_ident.to_string(); }

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
