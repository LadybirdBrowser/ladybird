/*
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class ShorthandStyleValue final : public StyleValueWithDefaultOperators<ShorthandStyleValue> {
public:
    static ValueComparingNonnullRefPtr<ShorthandStyleValue const> create(PropertyID shorthand, Vector<PropertyID> sub_properties, Vector<ValueComparingNonnullRefPtr<StyleValue const>> values)
    {
        return adopt_ref(*new ShorthandStyleValue(shorthand, move(sub_properties), move(values)));
    }
    virtual ~ShorthandStyleValue() override;

    Vector<PropertyID> const& sub_properties() const { return m_properties.sub_properties; }
    Vector<ValueComparingNonnullRefPtr<StyleValue const>> const& values() const { return m_properties.values; }

    ValueComparingRefPtr<StyleValue const> longhand(PropertyID) const;

    virtual void serialize(StringBuilder&, SerializationMode) const override;

    bool properties_equal(ShorthandStyleValue const& other) const { return m_properties == other.m_properties; }

private:
    ShorthandStyleValue(PropertyID shorthand, Vector<PropertyID> sub_properties, Vector<ValueComparingNonnullRefPtr<StyleValue const>> values);

    virtual void set_style_sheet(GC::Ptr<CSSStyleSheet>) override;

    struct Properties {
        PropertyID shorthand_property;
        Vector<PropertyID> sub_properties;
        Vector<ValueComparingNonnullRefPtr<StyleValue const>> values;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}
