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

    Optional<Color> to_color(ColorResolutionContext) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

private:
    friend class StyleValue;

    LightDarkStyleValue(ValueComparingNonnullRefPtr<StyleValue const> light, ValueComparingNonnullRefPtr<StyleValue const> dark)
        : ColorStyleValue(StyleValueFFI::rust_style_value_create_light_dark(false, 0, to_underlying(ColorSyntax::Modern), StyleValueFFI::rust_style_value_retain(light->rust_style_value_data()), StyleValueFFI::rust_style_value_retain(dark->rust_style_value_data())))
    {
    }

    explicit LightDarkStyleValue(StyleValueFFI::StyleValueData const* data)
        : ColorStyleValue(data)
    {
    }

    ValueComparingNonnullRefPtr<StyleValue const> light() const { return wrap_rust_child(m_value->light_dark.light); }
    ValueComparingNonnullRefPtr<StyleValue const> dark() const { return wrap_rust_child(m_value->light_dark.dark); }
};

} // Web::CSS
