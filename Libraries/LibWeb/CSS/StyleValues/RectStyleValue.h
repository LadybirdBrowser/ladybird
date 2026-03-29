/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/EdgeRect.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class RectStyleValue : public StyleValueWithDefaultOperators<RectStyleValue> {
public:
    static ValueComparingNonnullRefPtr<RectStyleValue const> create(NonnullRefPtr<StyleValue const> top, NonnullRefPtr<StyleValue const> right, NonnullRefPtr<StyleValue const> bottom, NonnullRefPtr<StyleValue const> left);
    virtual ~RectStyleValue() override = default;

    NonnullRefPtr<StyleValue const> top() const { return m_top; }
    NonnullRefPtr<StyleValue const> right() const { return m_right; }
    NonnullRefPtr<StyleValue const> bottom() const { return m_bottom; }
    NonnullRefPtr<StyleValue const> left() const { return m_left; }

    EdgeRect rect() const { return { LengthOrAuto::from_style_value(m_top, {}), LengthOrAuto::from_style_value(m_right, {}), LengthOrAuto::from_style_value(m_bottom, {}), LengthOrAuto::from_style_value(m_left, {}) }; }
    virtual void serialize(StringBuilder&, SerializationMode) const override;

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    bool properties_equal(RectStyleValue const& other) const
    {
        return m_top == other.m_top
            && m_right == other.m_right
            && m_bottom == other.m_bottom
            && m_left == other.m_left;
    }

    virtual bool is_computationally_independent() const override
    {
        return m_top->is_computationally_independent()
            && m_right->is_computationally_independent()
            && m_bottom->is_computationally_independent()
            && m_left->is_computationally_independent();
    }

private:
    explicit RectStyleValue(NonnullRefPtr<StyleValue const> top, NonnullRefPtr<StyleValue const> right, NonnullRefPtr<StyleValue const> bottom, NonnullRefPtr<StyleValue const> left)
        : StyleValueWithDefaultOperators(Type::Rect)
        , m_top(move(top))
        , m_right(move(right))
        , m_bottom(move(bottom))
        , m_left(move(left))
    {
    }

    ValueComparingNonnullRefPtr<StyleValue const> m_top;
    ValueComparingNonnullRefPtr<StyleValue const> m_right;
    ValueComparingNonnullRefPtr<StyleValue const> m_bottom;
    ValueComparingNonnullRefPtr<StyleValue const> m_left;
};

}
