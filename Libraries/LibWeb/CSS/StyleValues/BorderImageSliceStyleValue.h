/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

class BorderImageSliceStyleValue final : public StyleValueWithDefaultOperators<BorderImageSliceStyleValue> {
public:
    static ValueComparingNonnullRefPtr<BorderImageSliceStyleValue const> create(ValueComparingNonnullRefPtr<CSSStyleValue const> top, ValueComparingNonnullRefPtr<CSSStyleValue const> right, ValueComparingNonnullRefPtr<CSSStyleValue const> bottom, ValueComparingNonnullRefPtr<CSSStyleValue const> left, bool fill)
    {
        return adopt_ref(*new (nothrow) BorderImageSliceStyleValue(top, right, bottom, left, fill));
    }

    virtual ~BorderImageSliceStyleValue() override = default;

    ValueComparingNonnullRefPtr<CSSStyleValue const> top() const { return m_properties.top; }
    ValueComparingNonnullRefPtr<CSSStyleValue const> left() const { return m_properties.left; }
    ValueComparingNonnullRefPtr<CSSStyleValue const> bottom() const { return m_properties.bottom; }
    ValueComparingNonnullRefPtr<CSSStyleValue const> right() const { return m_properties.right; }

    bool fill() const { return m_properties.fill; }

    virtual String to_string(SerializationMode) const override;

    bool properties_equal(BorderImageSliceStyleValue const& other) const { return m_properties == other.m_properties; }

private:
    BorderImageSliceStyleValue(ValueComparingNonnullRefPtr<CSSStyleValue const> top, ValueComparingNonnullRefPtr<CSSStyleValue const> right, ValueComparingNonnullRefPtr<CSSStyleValue const> bottom, ValueComparingNonnullRefPtr<CSSStyleValue const> left, bool fill)
        : StyleValueWithDefaultOperators(Type::BorderImageSlice)
        , m_properties { .top = move(top), .right = move(right), .bottom = move(bottom), .left = move(left), .fill = fill }
    {
    }

    struct Properties {
        ValueComparingNonnullRefPtr<CSSStyleValue const> top;
        ValueComparingNonnullRefPtr<CSSStyleValue const> right;
        ValueComparingNonnullRefPtr<CSSStyleValue const> bottom;
        ValueComparingNonnullRefPtr<CSSStyleValue const> left;
        bool fill;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}
