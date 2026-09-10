/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>

namespace Web::CSS {

class ColorMixStyleValue final : public ColorStyleValue {
public:
    virtual ~ColorMixStyleValue() override = default;

    struct ColorMixComponent {
        ValueComparingNonnullRefPtr<StyleValue const> color;
        ValueComparingRefPtr<StyleValue const> percentage;
        bool operator==(ColorMixComponent const&) const = default;
    };

    static ValueComparingNonnullRefPtr<ColorMixStyleValue const> create(RefPtr<StyleValue const> color_interpolation_method, ColorMixComponent first_component, ColorMixComponent second_component);

private:
    friend class StyleValue;

    ColorMixStyleValue(RefPtr<StyleValue const> color_interpolation_method, ColorMixComponent first_component, ColorMixComponent second_component);
    explicit ColorMixStyleValue(StyleValueFFI::StyleValueData const*);

    static StyleValueFFI::StyleValueData const* make_color_mix_data(RefPtr<StyleValue const> const& color_interpolation_method, ColorMixComponent const& first_component, ColorMixComponent const& second_component)
    {
        auto retain = [](StyleValue const* value) {
            return value ? StyleValueFFI::rust_style_value_retain(value->rust_style_value_data()) : nullptr;
        };
        return StyleValueFFI::rust_style_value_create_color_mix(
            false, 0, to_underlying(ColorSyntax::Modern),
            retain(color_interpolation_method.ptr()),
            retain(first_component.color.ptr()), retain(first_component.percentage.ptr()),
            retain(second_component.color.ptr()), retain(second_component.percentage.ptr()));
    }
};

}
