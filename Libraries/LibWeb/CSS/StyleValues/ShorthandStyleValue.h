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

    Vector<PropertyID> sub_properties() const
    {
        auto const& list = m_value->shorthand.sub_properties;
        Vector<PropertyID> sub_properties;
        sub_properties.ensure_capacity(list.length);
        for (size_t i = 0; i < list.length; ++i)
            sub_properties.unchecked_append(static_cast<PropertyID>(list.pointer[i]));
        return sub_properties;
    }
    StyleValueVector values() const
    {
        return m_values;
    }

    ValueComparingRefPtr<StyleValue const> longhand(PropertyID) const;

    void serialize(StringBuilder&, SerializationMode) const;

    bool properties_equal(ShorthandStyleValue const& other) const
    {
        if (shorthand_property() != other.shorthand_property() || size() != other.size())
            return false;
        for (size_t i = 0; i < size(); ++i) {
            if (sub_property_at(i) != other.sub_property_at(i))
                return false;
            if (value_at(i) != other.value_at(i))
                return false;
        }
        return true;
    }

private:
    explicit ShorthandStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::Shorthand, data)
    {
        auto const& values = m_value->shorthand.values;
        m_values.ensure_capacity(values.length);
        for (size_t i = 0; i < values.length; ++i) {
            auto const* child_data = static_cast<StyleValueFFI::StyleValueData const*>(values.pointer[i].pointer);
            m_values.unchecked_append(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(child_data)));
        }
    }

    ShorthandStyleValue(PropertyID shorthand, Vector<PropertyID> sub_properties, Vector<ValueComparingNonnullRefPtr<StyleValue const>> values);

    // NB: StyleValue dispatches operations by type tag, so it may call private impls.
    friend class StyleValue;
    void set_style_sheet(GC::Ptr<CSSStyleSheet>);

    static StyleValueFFI::StyleValueData const* make_shorthand_data(PropertyID shorthand, Vector<PropertyID> const& sub_properties, Vector<ValueComparingNonnullRefPtr<StyleValue const>> const& values)
    {
        Vector<StyleValueFFI::StyleValueData const*> pointers;
        pointers.ensure_capacity(values.size());
        for (auto const& value : values)
            pointers.unchecked_append(StyleValueFFI::rust_style_value_retain(value->rust_style_value_data()));
        Vector<u16> property_ids;
        property_ids.ensure_capacity(sub_properties.size());
        for (auto property : sub_properties)
            property_ids.unchecked_append(to_underlying(property));
        return StyleValueFFI::rust_style_value_create_shorthand(to_underlying(shorthand), property_ids.data(), property_ids.size(), pointers.data(), pointers.size());
    }

    size_t size() const { return m_value->shorthand.values.length; }

    PropertyID sub_property_at(size_t i) const
    {
        return static_cast<PropertyID>(m_value->shorthand.sub_properties.pointer[i]);
    }

    ValueComparingNonnullRefPtr<StyleValue const> value_at(size_t i) const
    {
        return m_values[i];
    }

    PropertyID shorthand_property() const { return static_cast<PropertyID>(m_value->shorthand.shorthand_property); }

    StyleValueVector m_values;
};

}
