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

    ValueComparingNonnullRefPtr<StyleValue const> top() const { return m_top; }
    ValueComparingNonnullRefPtr<StyleValue const> left() const { return m_left; }
    ValueComparingNonnullRefPtr<StyleValue const> bottom() const { return m_bottom; }
    ValueComparingNonnullRefPtr<StyleValue const> right() const { return m_right; }

    bool fill() const { return m_value->border_image_slice.fill; }

    void serialize(StringBuilder&, SerializationMode) const;

    bool properties_equal(BorderImageSliceStyleValue const& other) const { return top() == other.top() && right() == other.right() && bottom() == other.bottom() && left() == other.left() && fill() == other.fill(); }

private:
    friend class StyleValue;

    explicit BorderImageSliceStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::BorderImageSlice, data)
        , m_top(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(data->border_image_slice.top.pointer))))
        , m_right(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(data->border_image_slice.right.pointer))))
        , m_bottom(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(data->border_image_slice.bottom.pointer))))
        , m_left(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(data->border_image_slice.left.pointer))))
    {
    }

    BorderImageSliceStyleValue(ValueComparingNonnullRefPtr<StyleValue const> top, ValueComparingNonnullRefPtr<StyleValue const> right, ValueComparingNonnullRefPtr<StyleValue const> bottom, ValueComparingNonnullRefPtr<StyleValue const> left, bool fill)
        : StyleValueWithDefaultOperators(Type::BorderImageSlice, StyleValueFFI::rust_style_value_create_border_image_slice(StyleValueFFI::rust_style_value_retain(top->rust_style_value_data()), StyleValueFFI::rust_style_value_retain(right->rust_style_value_data()), StyleValueFFI::rust_style_value_retain(bottom->rust_style_value_data()), StyleValueFFI::rust_style_value_retain(left->rust_style_value_data()), fill))
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
