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
        return number_from_style_value(parameter_style_value(), {});
    }

    void serialize(StringBuilder&, SerializationMode) const;

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    bool properties_equal(SuperellipseStyleValue const& other) const { return parameter_style_value() == other.parameter_style_value(); }

private:
    explicit SuperellipseStyleValue(ValueComparingNonnullRefPtr<StyleValue const> const& parameter)
        : StyleValueWithDefaultOperators(Type::Superellipse, StyleValueFFI::rust_style_value_create_superellipse(&NonnullRefPtr<StyleValue const>(parameter).leak_ref()))
    {
    }

    ValueComparingNonnullRefPtr<StyleValue const> parameter_style_value() const { return *static_cast<StyleValue const*>(m_value->superellipse.parameter.pointer); }
};

}
