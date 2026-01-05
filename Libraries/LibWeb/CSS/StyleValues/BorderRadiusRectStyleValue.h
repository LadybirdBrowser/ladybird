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

    virtual String to_string(SerializationMode) const override;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    NonnullRefPtr<StyleValue const> top_left() const { return m_top_left; }
    NonnullRefPtr<StyleValue const> top_right() const { return m_top_right; }
    NonnullRefPtr<StyleValue const> bottom_right() const { return m_bottom_right; }
    NonnullRefPtr<StyleValue const> bottom_left() const { return m_bottom_left; }

    bool properties_equal(BorderRadiusRectStyleValue const& other) const
    {
        return m_top_left == other.m_top_left
            && m_top_right == other.m_top_right
            && m_bottom_right == other.m_bottom_right
            && m_bottom_left == other.m_bottom_left;
    }

private:
    BorderRadiusRectStyleValue(NonnullRefPtr<StyleValue const> top_left, NonnullRefPtr<StyleValue const> top_right, NonnullRefPtr<StyleValue const> bottom_right, NonnullRefPtr<StyleValue const> bottom_left)
        : StyleValueWithDefaultOperators(Type::BorderRadiusRect)
        , m_top_left(move(top_left))
        , m_top_right(move(top_right))
        , m_bottom_right(move(bottom_right))
        , m_bottom_left(move(bottom_left))
    {
    }

    ValueComparingNonnullRefPtr<StyleValue const> m_top_left;
    ValueComparingNonnullRefPtr<StyleValue const> m_top_right;
    ValueComparingNonnullRefPtr<StyleValue const> m_bottom_right;
    ValueComparingNonnullRefPtr<StyleValue const> m_bottom_left;
};

}
