/*
 * Copyright (c) 2024-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HWBColorStyleValue.h"
#include <AK/TypeCasts.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/RGBColorStyleValue.h>

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

ValueComparingNonnullRefPtr<StyleValue const> HWBColorStyleValue::absolutized(ComputationContext const& context) const
{
    auto absolutized_h = m_properties.h->absolutized(context);
    auto absolutized_w = m_properties.w->absolutized(context);
    auto absolutized_b = m_properties.b->absolutized(context);
    auto absolutized_alpha = m_properties.alpha->absolutized(context);

    // hwb() computes to rgb()
    // https://drafts.csswg.org/css-color-4/#resolving-sRGB-values
    auto resolved_h = resolve_hue(absolutized_h, {});
    auto resolved_w = resolve_with_reference_value(absolutized_w, 100.0, {});
    auto resolved_b = resolve_with_reference_value(absolutized_b, 100.0, {});
    auto resolved_alpha = resolve_alpha(absolutized_alpha, {});

    // These should all be computable at this point.
    if (!resolved_h.has_value() || !resolved_w.has_value() || !resolved_b.has_value() || !resolved_alpha.has_value())
        VERIFY_NOT_REACHED();

    // https://drafts.csswg.org/css-color-4/#hwb-to-rgb
    float whiteness = clamp(resolved_w.value() / 100.0f, 0.0f, 1.0f);
    float blackness = clamp(resolved_b.value() / 100.0f, 0.0f, 1.0f);

    // If the sum of whiteness + blackness is >= 1, the result is an achromatic (gray) color.
    if (whiteness + blackness >= 1.0f) {
        auto gray = NumberStyleValue::create(clamp(whiteness / (whiteness + blackness) * 255.0f, 0.0f, 255.0f));
        return RGBColorStyleValue::create(gray, gray, gray,
            NumberStyleValue::create(clamp(resolved_alpha.value(), 0.0f, 1.0f)),
            ColorSyntax::Legacy);
    }

    // Convert hue to RGB (using HSL with S=1, L=0.5)
    auto hue = fmodf(resolved_h.value(), 360.0f);
    if (hue < 0.0f)
        hue += 360.0f;

    auto hue_to_rgb = [](float h, float offset) {
        float k = fmodf(offset + h / 30.0f, 12.0f);
        return 0.5f - 0.5f * max(-1.0f, min(min(k - 3.0f, 9.0f - k), 1.0f));
    };

    // Apply whiteness and blackness: rgb = rgb * (1 - whiteness - blackness) + whiteness
    auto scale = 1.0f - whiteness - blackness;
    auto r = hue_to_rgb(hue, 0.0f) * scale + whiteness;
    auto g = hue_to_rgb(hue, 8.0f) * scale + whiteness;
    auto b = hue_to_rgb(hue, 4.0f) * scale + whiteness;

    return RGBColorStyleValue::create(
        NumberStyleValue::create(clamp(r * 255.0f, 0.0f, 255.0f)),
        NumberStyleValue::create(clamp(g * 255.0f, 0.0f, 255.0f)),
        NumberStyleValue::create(clamp(b * 255.0f, 0.0f, 255.0f)),
        NumberStyleValue::create(clamp(resolved_alpha.value(), 0.0f, 1.0f)),
        ColorSyntax::Legacy);
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
