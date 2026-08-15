/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

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

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

private:
    friend class StyleValue;

    explicit SuperellipseStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::Superellipse, data)
    {
    }

    explicit SuperellipseStyleValue(ValueComparingNonnullRefPtr<StyleValue const> const& parameter)
        : StyleValueWithDefaultOperators(Type::Superellipse, StyleValueFFI::rust_style_value_create_superellipse(StyleValueFFI::rust_style_value_retain(parameter->rust_style_value_data())))
    {
    }

    ValueComparingNonnullRefPtr<StyleValue const> parameter_style_value() const { return wrap_rust_child(m_value->superellipse.parameter); }
};

}
