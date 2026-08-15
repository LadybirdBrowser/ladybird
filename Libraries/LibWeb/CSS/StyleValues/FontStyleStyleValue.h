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
    ValueComparingRefPtr<StyleValue const> angle() const { return wrap_rust_child_or_null(m_value->font_style.angle_value); }

    int to_font_slope() const;

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const& computation_context) const;

private:
    friend class StyleValue;

    explicit FontStyleStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::FontStyle, data)
    {
    }

    FontStyleStyleValue(FontStyleKeyword, ValueComparingRefPtr<StyleValue const> angle_value);

    static StyleValueFFI::StyleValueData const* make_font_style_data(FontStyleKeyword font_style, ValueComparingRefPtr<StyleValue const> const& angle_value)
    {
        return StyleValueFFI::rust_style_value_create_font_style(to_underlying(font_style), angle_value ? StyleValueFFI::rust_style_value_retain(angle_value->rust_style_value_data()) : nullptr);
    }
};

}
