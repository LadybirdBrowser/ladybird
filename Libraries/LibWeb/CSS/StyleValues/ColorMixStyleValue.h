/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Percentage.h>
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

    bool equals(StyleValue const&) const;
    Optional<Color> to_color(ColorResolutionContext) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;
    void serialize(StringBuilder&, SerializationMode) const;

private:
    ColorMixStyleValue(RefPtr<StyleValue const> color_interpolation_method, ColorMixComponent first_component, ColorMixComponent second_component);

    static StyleValueFFI::StyleValueData* make_color_mix_data(RefPtr<StyleValue const> const& color_interpolation_method, ColorMixComponent const& first_component, ColorMixComponent const& second_component)
    {
        // The Rust allocation takes ownership of one strong reference to each non-null value.
        return StyleValueFFI::rust_style_value_create_color_mix(
            false, 0, to_underlying(ColorSyntax::Modern),
            retain_style_value_for_rust(color_interpolation_method.ptr()),
            retain_style_value_for_rust(first_component.color.ptr()), retain_style_value_for_rust(first_component.percentage.ptr()),
            retain_style_value_for_rust(second_component.color.ptr()), retain_style_value_for_rust(second_component.percentage.ptr()));
    }

    ValueComparingRefPtr<StyleValue const> color_interpolation_method_value() const { return static_cast<StyleValue const*>(m_value->color_mix.color_interpolation_method.pointer); }
    ColorMixComponent first_component() const
    {
        return { *static_cast<StyleValue const*>(m_value->color_mix.first_color.pointer),
            static_cast<StyleValue const*>(m_value->color_mix.first_percentage.pointer) };
    }
    ColorMixComponent second_component() const
    {
        return { *static_cast<StyleValue const*>(m_value->color_mix.second_color.pointer),
            static_cast<StyleValue const*>(m_value->color_mix.second_percentage.pointer) };
    }

    struct NormalizedPercentages {
        Percentage first_percentage;
        Percentage second_percentage;
        double alpha_multiplier;
    };
    static NormalizedPercentages normalize_percentage_pair(Optional<Percentage> p1, Optional<Percentage> p2);

    struct PercentageNormalizationResult {
        ValueComparingNonnullRefPtr<StyleValue const> p1;
        ValueComparingNonnullRefPtr<StyleValue const> p2;
        double alpha_multiplier;
    };
    PercentageNormalizationResult normalize_percentages(ComputationContext const&) const;
};

}
