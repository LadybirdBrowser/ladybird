/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/Enums.h>

namespace Web::CSS {

class FontStyleStyleValue final : public StyleValueWithDefaultOperators<FontStyleStyleValue> {
public:
    static ValueComparingNonnullRefPtr<FontStyleStyleValue const> create(FontStyle font_style, ValueComparingRefPtr<CSSStyleValue const> angle_value = {})
    {
        return adopt_ref(*new (nothrow) FontStyleStyleValue(font_style, angle_value));
    }

    virtual ~FontStyleStyleValue() override;

    FontStyle font_style() const { return m_font_style; }
    ValueComparingRefPtr<CSSStyleValue const> angle() const { return m_angle_value; }

    virtual String to_string(SerializationMode) const override;

    bool equals(CSSStyleValue const& other) const override
    {
        if (type() != other.type())
            return false;
        auto const& other_font_style = other.as_font_style();
        return m_font_style == other_font_style.m_font_style && m_angle_value == other_font_style.m_angle_value;
    }

    bool properties_equal(FontStyleStyleValue const& other) const { return m_font_style == other.m_font_style && m_angle_value == other.m_angle_value; }

private:
    FontStyleStyleValue(FontStyle, ValueComparingRefPtr<CSSStyleValue const> angle_value);

    FontStyle m_font_style;
    ValueComparingRefPtr<CSSStyleValue const> m_angle_value;
};

}
