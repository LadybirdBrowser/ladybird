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
};

}
