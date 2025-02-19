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
#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

enum class ColorSyntax : u8 {
    Legacy,
    Modern,
};

// https://drafts.css-houdini.org/css-typed-om-1/#csscolorvalue
class CSSColorValue : public CSSStyleValue {
public:
    static ValueComparingNonnullRefPtr<CSSColorValue> create_from_color(Color color, ColorSyntax color_syntax, Optional<FlyString> name = {});
    virtual ~CSSColorValue() override = default;

    virtual bool has_color() const override { return true; }

    enum class ColorType {
        RGB, // This is used by CSSRGB for rgb(...) and rgba(...).
        A98RGB,
        DisplayP3,
        HSL,
        HWB,
        Lab,
        LCH,
        OKLab,
        OKLCH,
        sRGB, // This is used by CSSColor for color(srgb ...).
        sRGBLinear,
        ProPhotoRGB,
        Rec2020,
        XYZD50,
        XYZD65,
        LightDark, // This is used by CSSLightDark for light-dark(..., ...).
    };
    ColorType color_type() const { return m_color_type; }
    ColorSyntax color_syntax() const { return m_color_syntax; }

protected:
    explicit CSSColorValue(ColorType color_type, ColorSyntax color_syntax)
        : CSSStyleValue(Type::Color)
        , m_color_type(color_type)
        , m_color_syntax(color_syntax)
    {
    }

    static Optional<double> resolve_hue(CSSStyleValue const&);
    static Optional<double> resolve_with_reference_value(CSSStyleValue const&, float one_hundred_percent_value);
    static Optional<double> resolve_alpha(CSSStyleValue const&);

    ColorType m_color_type;
    ColorSyntax m_color_syntax;
};

}
