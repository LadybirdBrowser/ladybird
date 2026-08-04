/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class OpacityValueStyleValue final : public StyleValueWithDefaultOperators<OpacityValueStyleValue> {
public:
    static ValueComparingNonnullRefPtr<OpacityValueStyleValue const> create(NonnullRefPtr<StyleValue const>&& value)
    {
        return adopt_ref(*new (nothrow) OpacityValueStyleValue(move(value)));
    }

    virtual ~OpacityValueStyleValue() override = default;

    void serialize(StringBuilder&, SerializationMode) const;

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    double resolved() const { return value()->as_number().number(); }

    GC::Ref<CSSStyleValue> reify(Utf16FlyString const& associated_property) const;

    bool properties_equal(OpacityValueStyleValue const& other) const { return value() == other.value(); }

private:
    friend class StyleValue;

    explicit OpacityValueStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::OpacityValue, data)
        , m_wrapped_value(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(data->opacity_value.value.pointer))))
    {
    }

    OpacityValueStyleValue(NonnullRefPtr<StyleValue const>&& value)
        : StyleValueWithDefaultOperators(Type::OpacityValue, StyleValueFFI::rust_style_value_create_opacity_value(StyleValueFFI::rust_style_value_retain(value->rust_style_value_data())))
        , m_wrapped_value(move(value))
    {
    }

    ValueComparingNonnullRefPtr<StyleValue const> value() const { return m_wrapped_value; }

    ValueComparingNonnullRefPtr<StyleValue const> m_wrapped_value;
};

}
