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

    void serialize(StringBuilder&, SerializationMode) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    ValueComparingNonnullRefPtr<StyleValue const> top_left() const { return *static_cast<StyleValue const*>(m_value->border_radius_rect.top_left.pointer); }
    ValueComparingNonnullRefPtr<StyleValue const> top_right() const { return *static_cast<StyleValue const*>(m_value->border_radius_rect.top_right.pointer); }
    ValueComparingNonnullRefPtr<StyleValue const> bottom_right() const { return *static_cast<StyleValue const*>(m_value->border_radius_rect.bottom_right.pointer); }
    ValueComparingNonnullRefPtr<StyleValue const> bottom_left() const { return *static_cast<StyleValue const*>(m_value->border_radius_rect.bottom_left.pointer); }

    bool properties_equal(BorderRadiusRectStyleValue const& other) const
    {
        return top_left() == other.top_left()
            && top_right() == other.top_right()
            && bottom_right() == other.bottom_right()
            && bottom_left() == other.bottom_left();
    }

private:
    BorderRadiusRectStyleValue(NonnullRefPtr<StyleValue const> top_left, NonnullRefPtr<StyleValue const> top_right, NonnullRefPtr<StyleValue const> bottom_right, NonnullRefPtr<StyleValue const> bottom_left)
        : StyleValueWithDefaultOperators(Type::BorderRadiusRect, StyleValueFFI::rust_style_value_create_border_radius_rect(&top_left.leak_ref(), &top_right.leak_ref(), &bottom_right.leak_ref(), &bottom_left.leak_ref()))
    {
    }
};

}
