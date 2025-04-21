/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CalculatedOr.h>
#include <LibWeb/CSS/StyleValues/CSSColorValue.h>

namespace Web::CSS {

class ColorMixStyleValue final : public CSSColorValue {
public:
    virtual ~ColorMixStyleValue() override = default;

    struct ColorInterpolationMethod {
        String color_space;
        Optional<HueInterpolationMethod> hue_interpolation_method;
        bool operator==(ColorInterpolationMethod const&) const = default;
    };

    struct ColorMixComponent {
        ValueComparingNonnullRefPtr<CSSStyleValue const> color;
        Optional<PercentageOrCalculated> percentage;
        bool operator==(ColorMixComponent const&) const = default;
    };

    static ValueComparingNonnullRefPtr<ColorMixStyleValue const> create(ColorInterpolationMethod, ColorMixComponent first_component, ColorMixComponent second_component);

    virtual bool equals(CSSStyleValue const&) const override;
    virtual Color to_color(Optional<Layout::NodeWithStyle const&>) const override;
    virtual String to_string(SerializationMode) const override;

private:
    struct Properties {
        ColorInterpolationMethod color_interpolation_method;
        ColorMixComponent first_component;
        ColorMixComponent second_component;
        bool operator==(Properties const&) const = default;
    };

    ColorMixStyleValue(ColorInterpolationMethod, ColorMixComponent first_component, ColorMixComponent second_component);

    struct PercentageNormalizationResult {
        Percentage p1;
        Percentage p2;
        double alpha_multiplier;
    };
    PercentageNormalizationResult normalize_percentages() const;

    Properties m_properties;
};

}
