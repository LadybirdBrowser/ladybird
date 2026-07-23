/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-color-5/#contrast-color
class ContrastColorStyleValue final : public ColorStyleValue {
public:
    virtual ~ContrastColorStyleValue() override = default;

    static ValueComparingNonnullRefPtr<ContrastColorStyleValue const> create(ValueComparingNonnullRefPtr<StyleValue const> color)
    {
        return adopt_ref(*new (nothrow) ContrastColorStyleValue(move(color)));
    }

    bool equals(StyleValue const&) const;
    Optional<Color> to_color(ColorResolutionContext) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;
    void serialize(StringBuilder&, SerializationMode) const;

private:
    friend class StyleValue;

    explicit ContrastColorStyleValue(ValueComparingNonnullRefPtr<StyleValue const> color)
        : ColorStyleValue(StyleValueFFI::rust_style_value_create_contrast_color(false, 0, to_underlying(ColorSyntax::Modern), StyleValueFFI::rust_style_value_retain(color->rust_style_value_data())))
        , m_color(move(color))
    {
    }

    explicit ContrastColorStyleValue(StyleValueFFI::StyleValueData const* data)
        : ColorStyleValue(data)
        , m_color(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(data->contrast_color.color.pointer))))
    {
    }

    ValueComparingNonnullRefPtr<StyleValue const> color() const { return m_color; }

    ValueComparingNonnullRefPtr<StyleValue const> m_color;
};

}
