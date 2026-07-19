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

    ColorInterpolationMethod color_interpolation_method() const
    {
        if (m_value->color_interpolation_method.is_polar) {
            return PolarColorInterpolationMethod {
                static_cast<PolarColorSpace>(m_value->color_interpolation_method.color_space),
                static_cast<HueInterpolationMethod>(m_value->color_interpolation_method.hue_interpolation_method),
            };
        }
        return static_cast<RectangularColorSpace>(m_value->color_interpolation_method.color_space);
    }

    void serialize(StringBuilder&, SerializationMode) const;

    bool properties_equal(ColorInterpolationMethodStyleValue const& other) const { return color_interpolation_method() == other.color_interpolation_method(); }

private:
    explicit ColorInterpolationMethodStyleValue(ColorInterpolationMethod color_space)
        : StyleValueWithDefaultOperators(Type::ColorInterpolationMethod, make_color_interpolation_method_data(color_space))
    {
    }

    static StyleValueFFI::StyleValueData* make_color_interpolation_method_data(ColorInterpolationMethod const& color_interpolation_method)
    {
        return color_interpolation_method.visit(
            [](RectangularColorSpace const& color_space) {
                return StyleValueFFI::rust_style_value_create_color_interpolation_method(false, to_underlying(color_space), 0);
            },
            [](PolarColorInterpolationMethod const& polar) {
                return StyleValueFFI::rust_style_value_create_color_interpolation_method(true, to_underlying(polar.color_space), to_underlying(polar.hue_interpolation_method));
            });
    }
};

}
