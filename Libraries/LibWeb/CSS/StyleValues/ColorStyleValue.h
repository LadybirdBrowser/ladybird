/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibGfx/Color.h>
#include <LibWeb/CSS/StyleValues/RustStyleValueHandle.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

enum class ColorSyntax : u8 {
    Legacy,
    Modern,
};

class ColorStyleValue : public StyleValue {
public:
    static ValueComparingNonnullRefPtr<ColorStyleValue const> create_from_color(Color color, ColorSyntax color_syntax, Optional<Utf16FlyString> name = {});
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
    };
    Optional<ColorType> color_type() const
    {
        if (!m_color_base_value->color.has_color_type)
            return {};
        return static_cast<ColorType>(m_color_base_value->color.color_type);
    }
    ColorSyntax color_syntax() const { return static_cast<ColorSyntax>(m_color_base_value->color.color_syntax); }

    static Optional<double> resolve_hue(StyleValue const&, CalculationResolutionContext const&);
    static Optional<double> resolve_with_reference_value(StyleValue const&, float one_hundred_percent_value, CalculationResolutionContext const&);
    static Optional<double> resolve_alpha(StyleValue const&, CalculationResolutionContext const&);

    static Optional<RelativeColorContext> extract_channels_in_color_space(StyleValue const& origin_color, ColorType target_color_type, ColorResolutionContext const&);

protected:
    ColorStyleValue(Optional<ColorType> color_type, ColorSyntax color_syntax, StyleValueFFI::StyleValueData* value)
        : StyleValue(Type::Color, value)
        , m_color_base_value(StyleValueFFI::rust_style_value_create_color(color_type.has_value(), color_type.has_value() ? static_cast<u8>(to_underlying(*color_type)) : 0, to_underlying(color_syntax)))
    {
    }

    void serialize_color_component(StringBuilder& builder, SerializationMode mode, StyleValue const& component, float one_hundred_percent_value, Optional<double> clamp_min = {}, Optional<double> clamp_max = {}) const;
    void serialize_alpha_component(StringBuilder& builder, SerializationMode mode, StyleValue const& component) const;
    void serialize_hue_component(StringBuilder& builder, SerializationMode mode, StyleValue const& component) const;
    // NB: Color subclasses hold their own value data handle as well; the two merge when the
    //     C++ shells are collapsed into a single handle type.
    RustStyleValueHandle m_color_base_value;
};

}
