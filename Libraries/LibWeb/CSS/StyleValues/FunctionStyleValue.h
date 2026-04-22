/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class FunctionStyleValue : public StyleValueWithDefaultOperators<FunctionStyleValue> {
public:
    static NonnullRefPtr<FunctionStyleValue> create(FlyString name, NonnullRefPtr<StyleValue const> value)
    {
        return adopt_ref(*new FunctionStyleValue(move(name), move(value)));
    }

    FlyString const& name() const { return m_name; }
    NonnullRefPtr<StyleValue const> const& value() const { return m_value; }

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;
    virtual void serialize(StringBuilder&, SerializationMode) const override;

    bool properties_equal(FunctionStyleValue const& other) const { return m_name == other.m_name && m_value == other.m_value; }

    virtual bool is_computationally_independent() const override { return m_value->is_computationally_independent(); }

private:
    FunctionStyleValue(FlyString name, NonnullRefPtr<StyleValue const> value)
        : StyleValueWithDefaultOperators(Type::Function)
        , m_name(move(name))
        , m_value(move(value))
    {
    }

    virtual ~FunctionStyleValue() override = default;

    FlyString m_name;
    ValueComparingNonnullRefPtr<StyleValue const> m_value;
};

}
