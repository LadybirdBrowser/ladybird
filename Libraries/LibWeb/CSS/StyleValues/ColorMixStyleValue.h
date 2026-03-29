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

    virtual bool equals(StyleValue const&) const override;
    virtual Optional<Color> to_color(ColorResolutionContext) const override;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;
    virtual void serialize(StringBuilder&, SerializationMode) const override;

    virtual bool is_computationally_independent() const override
    {
        return (!m_properties.color_interpolation_method || m_properties.color_interpolation_method->is_computationally_independent())
            && m_properties.first_component.color->is_computationally_independent()
            && m_properties.second_component.color->is_computationally_independent()
            && (!m_properties.first_component.percentage || m_properties.first_component.percentage->is_computationally_independent())
            && (!m_properties.second_component.percentage || m_properties.second_component.percentage->is_computationally_independent());
    }

private:
    struct Properties {
        ValueComparingRefPtr<StyleValue const> color_interpolation_method;
        ColorMixComponent first_component;
        ColorMixComponent second_component;
        bool operator==(Properties const&) const = default;
    };

    ColorMixStyleValue(RefPtr<StyleValue const> color_interpolation_method, ColorMixComponent first_component, ColorMixComponent second_component);

    struct PercentageNormalizationResult {
        ValueComparingNonnullRefPtr<StyleValue const> p1;
        ValueComparingNonnullRefPtr<StyleValue const> p2;
        double alpha_multiplier;
    };
    PercentageNormalizationResult normalize_percentages(ComputationContext const&) const;

    Properties m_properties;
};

}
