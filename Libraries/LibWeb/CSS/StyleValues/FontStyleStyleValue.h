/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class FontStyleStyleValue final : public StyleValueWithDefaultOperators<FontStyleStyleValue> {
public:
    static ValueComparingNonnullRefPtr<FontStyleStyleValue const> create(FontStyleKeyword font_style, ValueComparingRefPtr<StyleValue const> angle_value = {})
    {
        return adopt_ref(*new (nothrow) FontStyleStyleValue(font_style, angle_value));
    }

    virtual ~FontStyleStyleValue() override;

    FontStyleKeyword font_style() const { return static_cast<FontStyleKeyword>(m_value->font_style.font_style); }
    ValueComparingRefPtr<StyleValue const> angle() const { return static_cast<StyleValue const*>(m_value->font_style.angle_value.pointer); }

    int to_font_slope() const;

    void serialize(StringBuilder&, SerializationMode) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const& computation_context) const;

    bool equals(StyleValue const& other) const
    {
        if (type() != other.type())
            return false;
        auto const& other_font_style = other.as_font_style();
        return font_style() == other_font_style.font_style() && angle() == other_font_style.angle();
    }

    bool properties_equal(FontStyleStyleValue const& other) const { return font_style() == other.font_style() && angle() == other.angle(); }

private:
    FontStyleStyleValue(FontStyleKeyword, ValueComparingRefPtr<StyleValue const> angle_value);

    static StyleValueFFI::StyleValueData* make_font_style_data(FontStyleKeyword font_style, ValueComparingRefPtr<StyleValue const> const& angle_value)
    {
        // The Rust allocation takes ownership of one strong reference to the angle value.
        if (angle_value)
            angle_value->ref();
        return StyleValueFFI::rust_style_value_create_font_style(to_underlying(font_style), angle_value.ptr());
    }
};

}
