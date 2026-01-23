/*
 * Copyright (c) 2025, Ladybird contributors
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LightDarkStyleValue.h"

namespace Web::CSS {

Optional<Color> LightDarkStyleValue::to_color(ColorResolutionContext color_resolution_context) const
{
    if (color_resolution_context.color_scheme == PreferredColorScheme::Dark)
        return m_properties.dark->to_color(color_resolution_context);

    return m_properties.light->to_color(color_resolution_context);
}

ValueComparingNonnullRefPtr<StyleValue const> LightDarkStyleValue::absolutized(ComputationContext const& context) const
{
    if (!context.color_scheme.has_value())
        return *this;

    if (context.color_scheme == PreferredColorScheme::Dark)
        return m_properties.dark->absolutized(context);

    return m_properties.light->absolutized(context);
}

bool LightDarkStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_color = other.as_color();
    if (color_type() != other_color.color_type())
        return false;
    auto const& other_light_dark = as<LightDarkStyleValue>(other_color);
    return m_properties == other_light_dark.m_properties;
}

void LightDarkStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    // FIXME: We don't have enough information to determine the computed value here.
    builder.append("light-dark("sv);
    m_properties.light->serialize(builder, mode);
    builder.append(", "sv);
    m_properties.dark->serialize(builder, mode);
    builder.append(')');
}

}
