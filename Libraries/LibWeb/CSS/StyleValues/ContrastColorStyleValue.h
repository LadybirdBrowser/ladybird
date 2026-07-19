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
    explicit ContrastColorStyleValue(ValueComparingNonnullRefPtr<StyleValue const> color)
        : ColorStyleValue(StyleValueFFI::rust_style_value_create_contrast_color(false, 0, to_underlying(ColorSyntax::Modern), &color.leak_ref()))
    {
    }

    ValueComparingNonnullRefPtr<StyleValue const> color() const { return *static_cast<StyleValue const*>(m_value->contrast_color.color.pointer); }
};

}
