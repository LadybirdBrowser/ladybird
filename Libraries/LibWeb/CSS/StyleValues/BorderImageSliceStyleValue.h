/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/RustStyleValueHandle.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class BorderImageSliceStyleValue final : public StyleValueWithDefaultOperators<BorderImageSliceStyleValue> {
public:
    static ValueComparingNonnullRefPtr<BorderImageSliceStyleValue const> create(ValueComparingNonnullRefPtr<StyleValue const> top, ValueComparingNonnullRefPtr<StyleValue const> right, ValueComparingNonnullRefPtr<StyleValue const> bottom, ValueComparingNonnullRefPtr<StyleValue const> left, bool fill)
    {
        return adopt_ref(*new (nothrow) BorderImageSliceStyleValue(top, right, bottom, left, fill));
    }

    virtual ~BorderImageSliceStyleValue() override = default;

    ValueComparingNonnullRefPtr<StyleValue const> top() const { return *static_cast<StyleValue const*>(m_value->border_image_slice.top.pointer); }
    ValueComparingNonnullRefPtr<StyleValue const> left() const { return *static_cast<StyleValue const*>(m_value->border_image_slice.left.pointer); }
    ValueComparingNonnullRefPtr<StyleValue const> bottom() const { return *static_cast<StyleValue const*>(m_value->border_image_slice.bottom.pointer); }
    ValueComparingNonnullRefPtr<StyleValue const> right() const { return *static_cast<StyleValue const*>(m_value->border_image_slice.right.pointer); }

    bool fill() const { return m_value->border_image_slice.fill; }

    virtual void serialize(StringBuilder&, SerializationMode) const override;

    bool properties_equal(BorderImageSliceStyleValue const& other) const { return top() == other.top() && right() == other.right() && bottom() == other.bottom() && left() == other.left() && fill() == other.fill(); }

    virtual bool is_computationally_independent() const override
    {
        return top()->is_computationally_independent()
            && right()->is_computationally_independent()
            && bottom()->is_computationally_independent()
            && left()->is_computationally_independent();
    }

private:
    BorderImageSliceStyleValue(ValueComparingNonnullRefPtr<StyleValue const> top, ValueComparingNonnullRefPtr<StyleValue const> right, ValueComparingNonnullRefPtr<StyleValue const> bottom, ValueComparingNonnullRefPtr<StyleValue const> left, bool fill)
        : StyleValueWithDefaultOperators(Type::BorderImageSlice)
        , m_value(StyleValueFFI::rust_style_value_create_border_image_slice(&top.leak_ref(), &right.leak_ref(), &bottom.leak_ref(), &left.leak_ref(), fill))
    {
    }

    RustStyleValueHandle m_value;
};

}
