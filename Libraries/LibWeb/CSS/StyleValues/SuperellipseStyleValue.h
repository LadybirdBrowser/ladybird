/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class SuperellipseStyleValue final : public StyleValueWithDefaultOperators<SuperellipseStyleValue> {
public:
    static ValueComparingNonnullRefPtr<SuperellipseStyleValue const> create(ValueComparingNonnullRefPtr<StyleValue const> const& parameter)
    {
        return adopt_ref(*new (nothrow) SuperellipseStyleValue(parameter));
    }
    virtual ~SuperellipseStyleValue() override = default;

    // NOTE: This function can only be called after absolutization
    double parameter() const
    {
        if (m_parameter->is_calculated())
            return m_parameter->as_calculated().resolve_number({}).value();

        return m_parameter->as_number().number();
    }

    virtual void serialize(StringBuilder&, SerializationMode) const override;

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    bool properties_equal(SuperellipseStyleValue const& other) const { return m_parameter == other.m_parameter; }

private:
    explicit SuperellipseStyleValue(ValueComparingNonnullRefPtr<StyleValue const> const& parameter)
        : StyleValueWithDefaultOperators(Type::Superellipse)
        , m_parameter(parameter)
    {
    }

    ValueComparingNonnullRefPtr<StyleValue const> m_parameter;
};

}
