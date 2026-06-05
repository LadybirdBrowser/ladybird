/*
 * Copyright (c) 2026, Ladybird contributors
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

    virtual bool equals(StyleValue const&) const override;
    virtual Optional<Color> to_color(ColorResolutionContext) const override;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;
    virtual void serialize(StringBuilder&, SerializationMode) const override;

    virtual bool is_computationally_independent() const override
    {
        return m_color->is_computationally_independent();
    }

private:
    explicit ContrastColorStyleValue(ValueComparingNonnullRefPtr<StyleValue const> color)
        : ColorStyleValue({}, ColorSyntax::Modern)
        , m_color(move(color))
    {
    }

    ValueComparingNonnullRefPtr<StyleValue const> m_color;
};

}
