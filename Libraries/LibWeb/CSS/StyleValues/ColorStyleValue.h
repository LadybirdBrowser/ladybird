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
    // AD-HOC: Every color variant payload starts with the same ColorBase prefix, so the base
    //         class reads it through one arm without knowing which color variant it has. The
    //         static_asserts in ColorStyleValue.cpp keep the prefixes in place.
    Optional<ColorType> color_type() const
    {
        auto const& color_base = m_value->color_function.color_base;
        if (!color_base.has_color_type)
            return {};
        return static_cast<ColorType>(color_base.color_type);
    }
    ColorSyntax color_syntax() const { return static_cast<ColorSyntax>(m_value->color_function.color_base.color_syntax); }

    void serialize(StringBuilder&, SerializationMode) const;
    bool equals(StyleValue const& other) const;
    Optional<Color> to_color(ColorResolutionContext) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    static Optional<double> resolve_hue(StyleValue const&, CalculationResolutionContext const&);
    static Optional<double> resolve_with_reference_value(StyleValue const&, float one_hundred_percent_value, CalculationResolutionContext const&);
    static Optional<double> resolve_alpha(StyleValue const&, CalculationResolutionContext const&);

    static Optional<RelativeColorContext> extract_channels_in_color_space(StyleValue const& origin_color, ColorType target_color_type, ColorResolutionContext const&);

protected:
    explicit ColorStyleValue(StyleValueFFI::StyleValueData* value)
        : StyleValue(Type::Color, value)
    {
    }

    // Packs the optional color type for a color creator's ColorBase arguments.
    static u8 color_type_byte(Optional<ColorType> color_type) { return color_type.has_value() ? static_cast<u8>(to_underlying(*color_type)) : 0; }

    void serialize_color_component(StringBuilder& builder, SerializationMode mode, StyleValue const& component, float one_hundred_percent_value, Optional<double> clamp_min = {}, Optional<double> clamp_max = {}) const;
    void serialize_alpha_component(StringBuilder& builder, SerializationMode mode, StyleValue const& component) const;
    void serialize_hue_component(StringBuilder& builder, SerializationMode mode, StyleValue const& component) const;
};

}
