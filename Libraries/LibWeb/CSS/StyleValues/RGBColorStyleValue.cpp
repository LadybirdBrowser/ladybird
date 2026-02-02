/*
 * Copyright (c) 2024-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "RGBColorStyleValue.h"
#include <AK/TypeCasts.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>

namespace Web::CSS {

Optional<Color> RGBColorStyleValue::to_color(ColorResolutionContext color_resolution_context) const
{
    auto resolve_rgb_to_u8 = [&color_resolution_context](StyleValue const& style_value) -> Optional<u8> {
        // <number> | <percentage> | none
        auto normalized = [](double number) {
            if (isnan(number))
                number = 0;
            return llround(clamp(number, 0.0, 255.0));
        };

        if (style_value.is_number())
            return normalized(style_value.as_number().number());

        if (style_value.is_percentage())
            return normalized(style_value.as_percentage().raw_value() * 255 / 100);

        if (style_value.is_calculated()) {
            auto const& calculated = style_value.as_calculated();
            if (calculated.resolves_to_number()) {
                auto maybe_number = calculated.resolve_number(color_resolution_context.calculation_resolution_context);

                if (!maybe_number.has_value())
                    return {};

                return normalized(maybe_number.value());
            }

            if (calculated.resolves_to_percentage()) {
                auto maybe_percentage = calculated.resolve_percentage(color_resolution_context.calculation_resolution_context);

                if (!maybe_percentage.has_value())
                    return {};

                return normalized(maybe_percentage.value().value() * 255 / 100);
            }
        }

        return 0;
    };

    auto resolve_alpha_to_u8 = [&color_resolution_context](StyleValue const& style_value) -> Optional<u8> {
        auto alpha_0_1 = resolve_alpha(style_value, color_resolution_context.calculation_resolution_context);
        if (alpha_0_1.has_value())
            return llround(clamp(alpha_0_1.value() * 255.0f, 0.0f, 255.0f));
        return {};
    };

    auto r_val = resolve_rgb_to_u8(m_properties.r);
    auto g_val = resolve_rgb_to_u8(m_properties.g);
    auto b_val = resolve_rgb_to_u8(m_properties.b);
    auto alpha_val = resolve_alpha_to_u8(m_properties.alpha);

    if (!r_val.has_value() || !g_val.has_value() || !b_val.has_value() || !alpha_val.has_value())
        return {};

    return Color(r_val.value(), g_val.value(), b_val.value(), alpha_val.value());
}

ValueComparingNonnullRefPtr<StyleValue const> RGBColorStyleValue::absolutized(ComputationContext const& context) const
{
    auto r = m_properties.r->absolutized(context);
    auto g = m_properties.g->absolutized(context);
    auto b = m_properties.b->absolutized(context);
    auto alpha = m_properties.alpha->absolutized(context);
    if (r == m_properties.r && g == m_properties.g && b == m_properties.b && alpha == m_properties.alpha)
        return *this;
    return RGBColorStyleValue::create(move(r), move(g), move(b), move(alpha), color_syntax());
}

bool RGBColorStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_color = other.as_color();
    if (color_type() != other_color.color_type())
        return false;
    auto const& other_rgb = static_cast<RGBColorStyleValue const&>(other_color);
    return m_properties == other_rgb.m_properties;
}

// https://www.w3.org/TR/css-color-4/#serializing-sRGB-values
void RGBColorStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    if (mode != SerializationMode::ResolvedValue && m_properties.name.has_value()) {
        for (auto c : m_properties.name.value().bytes_as_string_view())
            builder.append(AK::to_ascii_lowercase(c));
        return;
    }

    if (auto color = to_color({}); color.has_value()) {
        builder.append(color->serialize_a_srgb_value());
        return;
    }

    builder.append("rgb("sv);
    serialize_color_component(builder, mode, m_properties.r, 255, 0, 255);
    builder.append(" "sv);
    serialize_color_component(builder, mode, m_properties.g, 255, 0, 255);
    builder.append(" "sv);
    serialize_color_component(builder, mode, m_properties.b, 255, 0, 255);
    if ((!m_properties.alpha->is_number() || m_properties.alpha->as_number().number() < 1) && (!m_properties.alpha->is_percentage() || m_properties.alpha->as_percentage().percentage().as_fraction() < 1)) {
        builder.append(" / "sv);
        serialize_alpha_component(builder, mode, m_properties.alpha);
    }
    builder.append(")"sv);
}

}
