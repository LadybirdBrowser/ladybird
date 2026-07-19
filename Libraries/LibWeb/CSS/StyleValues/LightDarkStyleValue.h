/*
 * Copyright (c) 2025-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-color-5/#funcdef-light-dark
class LightDarkStyleValue final : public ColorStyleValue {
public:
    virtual ~LightDarkStyleValue() override = default;

    static ValueComparingNonnullRefPtr<LightDarkStyleValue const> create(ValueComparingNonnullRefPtr<StyleValue const> light, ValueComparingNonnullRefPtr<StyleValue const> dark)
    {
        return AK::adopt_ref(*new (nothrow) LightDarkStyleValue(move(light), move(dark)));
    }

    bool equals(StyleValue const&) const;
    Optional<Color> to_color(ColorResolutionContext) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;
    void serialize(StringBuilder&, SerializationMode) const;
    bool is_computationally_independent() const { return false; }

private:
    LightDarkStyleValue(ValueComparingNonnullRefPtr<StyleValue const> light, ValueComparingNonnullRefPtr<StyleValue const> dark)
        : ColorStyleValue(StyleValueFFI::rust_style_value_create_light_dark(false, 0, to_underlying(ColorSyntax::Modern), &light.leak_ref(), &dark.leak_ref()))
    {
    }

    ValueComparingNonnullRefPtr<StyleValue const> light() const { return *static_cast<StyleValue const*>(m_value->light_dark.light.pointer); }
    ValueComparingNonnullRefPtr<StyleValue const> dark() const { return *static_cast<StyleValue const*>(m_value->light_dark.dark.pointer); }
};

} // Web::CSS
