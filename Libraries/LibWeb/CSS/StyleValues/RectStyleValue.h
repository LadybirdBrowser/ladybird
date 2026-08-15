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

    ValueComparingNonnullRefPtr<StyleValue const> top() const { return wrap_rust_child(m_value->rect.top); }
    ValueComparingNonnullRefPtr<StyleValue const> right() const { return wrap_rust_child(m_value->rect.right); }
    ValueComparingNonnullRefPtr<StyleValue const> bottom() const { return wrap_rust_child(m_value->rect.bottom); }
    ValueComparingNonnullRefPtr<StyleValue const> left() const { return wrap_rust_child(m_value->rect.left); }

    EdgeRect rect() const { return { LengthOrAuto::from_style_value(top(), {}), LengthOrAuto::from_style_value(right(), {}), LengthOrAuto::from_style_value(bottom(), {}), LengthOrAuto::from_style_value(left(), {}) }; }

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

private:
    friend class StyleValue;

    explicit RectStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::Rect, data)
    {
    }

    explicit RectStyleValue(NonnullRefPtr<StyleValue const> top, NonnullRefPtr<StyleValue const> right, NonnullRefPtr<StyleValue const> bottom, NonnullRefPtr<StyleValue const> left)
        : StyleValueWithDefaultOperators(Type::Rect, StyleValueFFI::rust_style_value_create_rect(StyleValueFFI::rust_style_value_retain(top->rust_style_value_data()), StyleValueFFI::rust_style_value_retain(right->rust_style_value_data()), StyleValueFFI::rust_style_value_retain(bottom->rust_style_value_data()), StyleValueFFI::rust_style_value_retain(left->rust_style_value_data())))
    {
    }
};

}
