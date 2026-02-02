/*
 * Copyright (c) 2024-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HSLColorStyleValue.h"
#include <AK/TypeCasts.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/RGBColorStyleValue.h>

namespace Web::CSS {

Optional<Color> HSLColorStyleValue::to_color(ColorResolutionContext color_resolution_context) const
{
    auto h_val = resolve_hue(m_properties.h, color_resolution_context.calculation_resolution_context);
    auto s_val = resolve_with_reference_value(m_properties.s, 100.0, color_resolution_context.calculation_resolution_context);
    auto l_val = resolve_with_reference_value(m_properties.l, 100.0, color_resolution_context.calculation_resolution_context);
    auto alpha_val = resolve_alpha(m_properties.alpha, color_resolution_context.calculation_resolution_context);

    if (!h_val.has_value() || !s_val.has_value() || !l_val.has_value() || !alpha_val.has_value())
        return {};

    return Color::from_hsla(h_val.value(), s_val.value() / 100.0f, l_val.value() / 100.0f, alpha_val.value());
}

ValueComparingNonnullRefPtr<StyleValue const> HSLColorStyleValue::absolutized(ComputationContext const& context) const
{
    auto absolutized_h = m_properties.h->absolutized(context);
    auto absolutized_s = m_properties.s->absolutized(context);
    auto absolutized_l = m_properties.l->absolutized(context);
    auto absolutized_alpha = m_properties.alpha->absolutized(context);

    // hsl() computes to rgb()
    // https://drafts.csswg.org/css-color-4/#resolving-sRGB-values
    auto resolved_h = resolve_hue(absolutized_h, {});
    auto resolved_s = resolve_with_reference_value(absolutized_s, 100.0, {});
    auto resolved_l = resolve_with_reference_value(absolutized_l, 100.0, {});
    auto resolved_alpha = resolve_alpha(absolutized_alpha, {});

    // These should all be computable at this point.
    if (!resolved_h.has_value() || !resolved_s.has_value() || !resolved_l.has_value() || !resolved_alpha.has_value())
        VERIFY_NOT_REACHED();

    // https://drafts.csswg.org/css-color-4/#hsl-to-rgb
    auto hue = fmod(resolved_h.value(), 360.0);
    if (hue < 0.0)
        hue += 360.0;
    auto saturation = clamp(resolved_s.value() / 100.0, 0.0, 1.0);
    auto lightness = clamp(resolved_l.value() / 100.0, 0.0, 1.0);

    auto to_rgb = [](auto h, auto s, auto l, auto offset) {
        auto k = fmod(offset + h / 30.0, 12.0);
        auto a = s * min(l, 1.0 - l);
        return l - a * max(-1.0, min(min(k - 3.0, 9.0 - k), 1.0));
    };

    auto r = to_rgb(hue, saturation, lightness, 0.0);
    auto g = to_rgb(hue, saturation, lightness, 8.0);
    auto b = to_rgb(hue, saturation, lightness, 4.0);

    return RGBColorStyleValue::create(
        NumberStyleValue::create(clamp(r * 255.0, 0, 255)),
        NumberStyleValue::create(clamp(g * 255.0, 0, 255)),
        NumberStyleValue::create(clamp(b * 255.0, 0, 255)),
        NumberStyleValue::create(clamp(resolved_alpha.value(), 0.0, 1.0)),
        ColorSyntax::Legacy);
}

bool HSLColorStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_color = other.as_color();
    if (color_type() != other_color.color_type())
        return false;
    auto const& other_hsl = as<HSLColorStyleValue>(other_color);
    return m_properties == other_hsl.m_properties;
}

// https://www.w3.org/TR/css-color-4/#serializing-sRGB-values
void HSLColorStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    if (auto color = to_color({}); color.has_value()) {
        builder.append(color->serialize_a_srgb_value());
        return;
    }

    builder.append("hsl("sv);
    serialize_hue_component(builder, mode, m_properties.h);
    builder.append(" "sv);
    serialize_color_component(builder, mode, m_properties.s, 100, 0);
    builder.append(" "sv);
    serialize_color_component(builder, mode, m_properties.l, 100, 0);
    if ((!m_properties.alpha->is_number() || m_properties.alpha->as_number().number() < 1) && (!m_properties.alpha->is_percentage() || m_properties.alpha->as_percentage().percentage().as_fraction() < 1)) {
        builder.append(" / "sv);
        serialize_alpha_component(builder, mode, m_properties.alpha);
    }

    builder.append(")"sv);
}

}
