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

    virtual void serialize(StringBuilder&, SerializationMode) const override;

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    double resolved() const { return m_value->as_number().number(); }

    virtual GC::Ref<CSSStyleValue> reify(JS::Realm& realm, FlyString const& associated_property) const override;

    bool properties_equal(OpacityValueStyleValue const& other) const { return m_value == other.m_value; }

    virtual bool is_computationally_independent() const override { return m_value->is_computationally_independent(); }

private:
    OpacityValueStyleValue(NonnullRefPtr<StyleValue const>&& value)
        : StyleValueWithDefaultOperators(Type::OpacityValue)
        , m_value(move(value))
    {
    }

    ValueComparingNonnullRefPtr<StyleValue const> m_value;
};

}
