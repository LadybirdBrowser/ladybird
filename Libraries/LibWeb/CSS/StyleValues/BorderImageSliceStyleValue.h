/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class BorderImageSliceStyleValue final : public StyleValueWithDefaultOperators<BorderImageSliceStyleValue> {
public:
    static ValueComparingNonnullRefPtr<BorderImageSliceStyleValue const> create(ValueComparingNonnullRefPtr<StyleValue const> top, ValueComparingNonnullRefPtr<StyleValue const> right, ValueComparingNonnullRefPtr<StyleValue const> bottom, ValueComparingNonnullRefPtr<StyleValue const> left, bool fill)
    {
        return adopt_ref(*new (nothrow) BorderImageSliceStyleValue(top, right, bottom, left, fill));
    }

    virtual ~BorderImageSliceStyleValue() override = default;

    ValueComparingNonnullRefPtr<StyleValue const> top() const { return wrap_rust_child(m_value->border_image_slice.top); }
    ValueComparingNonnullRefPtr<StyleValue const> left() const { return wrap_rust_child(m_value->border_image_slice.left); }
    ValueComparingNonnullRefPtr<StyleValue const> bottom() const { return wrap_rust_child(m_value->border_image_slice.bottom); }
    ValueComparingNonnullRefPtr<StyleValue const> right() const { return wrap_rust_child(m_value->border_image_slice.right); }

    bool fill() const { return m_value->border_image_slice.fill; }

private:
    friend class StyleValue;

    explicit BorderImageSliceStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::BorderImageSlice, data)
    {
    }

    BorderImageSliceStyleValue(ValueComparingNonnullRefPtr<StyleValue const> top, ValueComparingNonnullRefPtr<StyleValue const> right, ValueComparingNonnullRefPtr<StyleValue const> bottom, ValueComparingNonnullRefPtr<StyleValue const> left, bool fill)
        : StyleValueWithDefaultOperators(Type::BorderImageSlice, StyleValueFFI::rust_style_value_create_border_image_slice(StyleValueFFI::rust_style_value_retain(top->rust_style_value_data()), StyleValueFFI::rust_style_value_retain(right->rust_style_value_data()), StyleValueFFI::rust_style_value_retain(bottom->rust_style_value_data()), StyleValueFFI::rust_style_value_retain(left->rust_style_value_data()), fill))
    {
    }
};

}
