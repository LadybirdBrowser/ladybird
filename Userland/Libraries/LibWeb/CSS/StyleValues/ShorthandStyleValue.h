/*
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

class ShorthandStyleValue final : public StyleValueWithDefaultOperators<ShorthandStyleValue> {
public:
    static ValueComparingNonnullRefPtr<ShorthandStyleValue> create(PropertyID shorthand, Vector<PropertyID> sub_properties, Vector<ValueComparingNonnullRefPtr<CSSStyleValue const>> values)
    {
        return adopt_ref(*new ShorthandStyleValue(shorthand, move(sub_properties), move(values)));
    }
    virtual ~ShorthandStyleValue() override;

    Vector<PropertyID> const& sub_properties() const { return m_properties.sub_properties; }
    Vector<ValueComparingNonnullRefPtr<CSSStyleValue const>> const& values() const { return m_properties.values; }

    ValueComparingRefPtr<CSSStyleValue const> longhand(PropertyID) const;

    virtual String to_string() const override;

    bool properties_equal(ShorthandStyleValue const& other) const { return m_properties == other.m_properties; }

private:
    ShorthandStyleValue(PropertyID shorthand, Vector<PropertyID> sub_properties, Vector<ValueComparingNonnullRefPtr<CSSStyleValue const>> values);

    struct Properties {
        PropertyID shorthand_property;
        Vector<PropertyID> sub_properties;
        Vector<ValueComparingNonnullRefPtr<CSSStyleValue const>> values;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}
