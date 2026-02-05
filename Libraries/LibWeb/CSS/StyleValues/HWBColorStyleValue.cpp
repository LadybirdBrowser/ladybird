/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HWBColorStyleValue.h"
#include <AK/TypeCasts.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>

namespace Web::CSS {

Optional<Color> HWBColorStyleValue::to_color(ColorResolutionContext color_resolution_context) const
{
    auto h_val = resolve_hue(m_properties.h, color_resolution_context.calculation_resolution_context);
    auto raw_w_value = resolve_with_reference_value(m_properties.w, 100.0, color_resolution_context.calculation_resolution_context);
    auto raw_b_value = resolve_with_reference_value(m_properties.b, 100.0, color_resolution_context.calculation_resolution_context);
    auto alpha_val = resolve_alpha(m_properties.alpha, color_resolution_context.calculation_resolution_context);

    if (!h_val.has_value() || !raw_w_value.has_value() || !raw_b_value.has_value() || !alpha_val.has_value())
        return {};

    auto w_val = clamp(raw_w_value.value(), 0, 100) / 100.0f;
    auto b_val = clamp(raw_b_value.value(), 0, 100) / 100.0f;

    if (w_val + b_val >= 1.0f) {
        auto to_byte = [](float value) {
            return round_to<u8>(clamp(value * 255.0f, 0.0f, 255.0f));
        };
        u8 gray = to_byte(w_val / (w_val + b_val));
        return Color(gray, gray, gray, to_byte(alpha_val.value()));
    }

    auto value = 1 - b_val;
    auto saturation = 1 - (w_val / value);
    return Color::from_hsv(h_val.value(), saturation, value).with_opacity(alpha_val.value());
}

bool HWBColorStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_color = other.as_color();
    if (color_type() != other_color.color_type())
        return false;
    auto const& other_hwb = as<HWBColorStyleValue>(other_color);
    return m_properties == other_hwb.m_properties;
}

// https://www.w3.org/TR/css-color-4/#serializing-sRGB-values
void HWBColorStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    if (auto color = to_color({}); color.has_value()) {
        builder.append(color->serialize_a_srgb_value());
        return;
    }

    builder.append("hwb("sv);
    serialize_hue_component(builder, mode, m_properties.h);
    builder.append(" "sv);
    serialize_color_component(builder, mode, m_properties.w, 100, 0);
    builder.append(" "sv);
    serialize_color_component(builder, mode, m_properties.b, 100, 0);
    if ((!m_properties.alpha->is_number() || m_properties.alpha->as_number().number() < 1) && (!m_properties.alpha->is_percentage() || m_properties.alpha->as_percentage().percentage().as_fraction() < 1)) {
        builder.append(" / "sv);
        serialize_alpha_component(builder, mode, m_properties.alpha);
    }
    builder.append(")"sv);
}

}
