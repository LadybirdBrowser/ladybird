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

    ValueComparingNonnullRefPtr<StyleValue const> top() const { return *static_cast<StyleValue const*>(m_value->rect.top.pointer); }
    ValueComparingNonnullRefPtr<StyleValue const> right() const { return *static_cast<StyleValue const*>(m_value->rect.right.pointer); }
    ValueComparingNonnullRefPtr<StyleValue const> bottom() const { return *static_cast<StyleValue const*>(m_value->rect.bottom.pointer); }
    ValueComparingNonnullRefPtr<StyleValue const> left() const { return *static_cast<StyleValue const*>(m_value->rect.left.pointer); }

    EdgeRect rect() const { return { LengthOrAuto::from_style_value(top(), {}), LengthOrAuto::from_style_value(right(), {}), LengthOrAuto::from_style_value(bottom(), {}), LengthOrAuto::from_style_value(left(), {}) }; }
    void serialize(StringBuilder&, SerializationMode) const;

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    bool properties_equal(RectStyleValue const& other) const
    {
        return top() == other.top()
            && right() == other.right()
            && bottom() == other.bottom()
            && left() == other.left();
    }

private:
    explicit RectStyleValue(NonnullRefPtr<StyleValue const> top, NonnullRefPtr<StyleValue const> right, NonnullRefPtr<StyleValue const> bottom, NonnullRefPtr<StyleValue const> left)
        : StyleValueWithDefaultOperators(Type::Rect, StyleValueFFI::rust_style_value_create_rect(&top.leak_ref(), &right.leak_ref(), &bottom.leak_ref(), &left.leak_ref()))
    {
    }
};

}
