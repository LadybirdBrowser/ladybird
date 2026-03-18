/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class ColorInterpolationMethodStyleValue final : public StyleValueWithDefaultOperators<ColorInterpolationMethodStyleValue> {
public:
    struct PolarColorInterpolationMethod {
        PolarColorSpace color_space;
        HueInterpolationMethod hue_interpolation_method { HueInterpolationMethod::Shorter };

        bool operator==(PolarColorInterpolationMethod const&) const = default;
    };
    using ColorInterpolationMethod = Variant<RectangularColorSpace, PolarColorInterpolationMethod>;

    static ColorInterpolationMethod default_color_interpolation_method(ColorSyntax color_syntax);

    static ValueComparingNonnullRefPtr<ColorInterpolationMethodStyleValue const> create(ColorInterpolationMethod color_space);

    virtual ~ColorInterpolationMethodStyleValue() override = default;

    ColorInterpolationMethod const& color_interpolation_method() const { return m_color_interpolation_method; }

    virtual void serialize(StringBuilder&, SerializationMode) const override;

    bool properties_equal(ColorInterpolationMethodStyleValue const& other) const { return m_color_interpolation_method == other.m_color_interpolation_method; }

private:
    explicit ColorInterpolationMethodStyleValue(ColorInterpolationMethod color_space)
        : StyleValueWithDefaultOperators(Type::ColorInterpolationMethod)
        , m_color_interpolation_method(move(color_space))
    {
    }

    ColorInterpolationMethod m_color_interpolation_method;
};

}
