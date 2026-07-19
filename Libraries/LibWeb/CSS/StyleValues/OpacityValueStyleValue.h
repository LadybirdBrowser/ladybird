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

    GC::Ref<CSSStyleValue> reify(JS::Realm& realm, Utf16FlyString const& associated_property) const;

    bool properties_equal(OpacityValueStyleValue const& other) const { return value() == other.value(); }

private:
    OpacityValueStyleValue(NonnullRefPtr<StyleValue const>&& value)
        : StyleValueWithDefaultOperators(Type::OpacityValue, StyleValueFFI::rust_style_value_create_opacity_value(&value.leak_ref()))
    {
    }

    ValueComparingNonnullRefPtr<StyleValue const> value() const { return *static_cast<StyleValue const*>(m_value->opacity_value.value.pointer); }
};

}
