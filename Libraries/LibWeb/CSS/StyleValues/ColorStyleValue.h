/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibGfx/Color.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

enum class ColorSyntax : u8 {
    Legacy,
    Modern,
};

class ColorStyleValue : public StyleValue {
public:
    static ValueComparingNonnullRefPtr<ColorStyleValue const> create_from_color(Gfx::Color color, ColorSyntax color_syntax, Optional<FlyString> name = {});
    virtual ~ColorStyleValue() override = default;

    virtual bool has_color() const override { return true; }

    enum class ColorType {
        RGB, // This is used by RGBColorStyleValue for rgb(...) and rgba(...).
        A98RGB,
        DisplayP3,
        DisplayP3Linear,
        HSL,
        HWB,
        Lab,
        LCH,
        OKLab,
        OKLCH,
        sRGB, // This is used by ColorFunctionStyleValue for color(srgb ...).
        sRGBLinear,
        ProPhotoRGB,
        Rec2020,
        XYZD50,
        XYZD65,
        LightDark, // This is used by LightDarkStyleValue for light-dark(..., ...).
        ColorMix,
    };
    ColorType color_type() const { return m_color_type; }
    ColorSyntax color_syntax() const { return m_color_syntax; }

protected:
    explicit ColorStyleValue(ColorType color_type, ColorSyntax color_syntax)
        : StyleValue(Type::Color)
        , m_color_type(color_type)
        , m_color_syntax(color_syntax)
    {
    }

    static Optional<double> resolve_hue(StyleValue const&, CalculationResolutionContext const&);
    static Optional<double> resolve_with_reference_value(StyleValue const&, float one_hundred_percent_value, CalculationResolutionContext const&);
    static Optional<double> resolve_alpha(StyleValue const&, CalculationResolutionContext const&);

    void serialize_color_component(StringBuilder& builder, SerializationMode mode, StyleValue const& component, float one_hundred_percent_value, Optional<double> clamp_min = {}, Optional<double> clamp_max = {}) const;
    void serialize_alpha_component(StringBuilder& builder, SerializationMode mode, StyleValue const& component) const;
    void serialize_hue_component(StringBuilder& builder, SerializationMode mode, StyleValue const& component) const;

    ColorType m_color_type;
    ColorSyntax m_color_syntax;
};

}
