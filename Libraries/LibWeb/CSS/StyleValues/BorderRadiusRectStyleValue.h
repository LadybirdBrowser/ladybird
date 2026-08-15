/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class BorderRadiusRectStyleValue final : public StyleValueWithDefaultOperators<BorderRadiusRectStyleValue> {
public:
    static ValueComparingNonnullRefPtr<BorderRadiusRectStyleValue const> create_zero()
    {
        return create(BorderRadiusStyleValue::create_zero(), BorderRadiusStyleValue::create_zero(), BorderRadiusStyleValue::create_zero(), BorderRadiusStyleValue::create_zero());
    }

    static ValueComparingNonnullRefPtr<BorderRadiusRectStyleValue const> create(NonnullRefPtr<StyleValue const> top_left, NonnullRefPtr<StyleValue const> top_right, NonnullRefPtr<StyleValue const> bottom_right, NonnullRefPtr<StyleValue const> bottom_left)
    {
        return adopt_ref(*new (nothrow) BorderRadiusRectStyleValue(move(top_left), move(top_right), move(bottom_right), move(bottom_left)));
    }

    virtual ~BorderRadiusRectStyleValue() override = default;

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    ValueComparingNonnullRefPtr<StyleValue const> top_left() const { return wrap_rust_child(m_value->border_radius_rect.top_left); }
    ValueComparingNonnullRefPtr<StyleValue const> top_right() const { return wrap_rust_child(m_value->border_radius_rect.top_right); }
    ValueComparingNonnullRefPtr<StyleValue const> bottom_right() const { return wrap_rust_child(m_value->border_radius_rect.bottom_right); }
    ValueComparingNonnullRefPtr<StyleValue const> bottom_left() const { return wrap_rust_child(m_value->border_radius_rect.bottom_left); }

private:
    friend class StyleValue;

    explicit BorderRadiusRectStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::BorderRadiusRect, data)
    {
    }

    BorderRadiusRectStyleValue(NonnullRefPtr<StyleValue const> top_left, NonnullRefPtr<StyleValue const> top_right, NonnullRefPtr<StyleValue const> bottom_right, NonnullRefPtr<StyleValue const> bottom_left)
        : StyleValueWithDefaultOperators(Type::BorderRadiusRect, StyleValueFFI::rust_style_value_create_border_radius_rect(StyleValueFFI::rust_style_value_retain(top_left->rust_style_value_data()), StyleValueFFI::rust_style_value_retain(top_right->rust_style_value_data()), StyleValueFFI::rust_style_value_retain(bottom_right->rust_style_value_data()), StyleValueFFI::rust_style_value_retain(bottom_left->rust_style_value_data())))
    {
    }
};

}
