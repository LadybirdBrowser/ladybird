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

    Optional<Color> to_color(ColorResolutionContext) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

private:
    friend class StyleValue;

    explicit ContrastColorStyleValue(ValueComparingNonnullRefPtr<StyleValue const> color)
        : ColorStyleValue(StyleValueFFI::rust_style_value_create_contrast_color(false, 0, to_underlying(ColorSyntax::Modern), StyleValueFFI::rust_style_value_retain(color->rust_style_value_data())))
    {
    }

    explicit ContrastColorStyleValue(StyleValueFFI::StyleValueData const* data)
        : ColorStyleValue(data)
    {
    }

    ValueComparingNonnullRefPtr<StyleValue const> color() const { return wrap_rust_child(m_value->contrast_color.color); }
};

}
