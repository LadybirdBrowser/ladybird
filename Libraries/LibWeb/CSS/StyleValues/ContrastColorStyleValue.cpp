/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleValues/ContrastColorStyleValue.h>

namespace Web::CSS {

Optional<Color> ContrastColorStyleValue::to_color(ColorResolutionContext color_resolution_context) const
{
    auto color = m_color->to_color(color_resolution_context);
    if (!color.has_value())
        return {};
    return color->suggested_foreground_color();
}

ValueComparingNonnullRefPtr<StyleValue const> ContrastColorStyleValue::absolutized(ComputationContext const& context) const
{
    ColorResolutionContext color_resolution_context {
        .color_scheme = context.color_scheme,
        .current_color = {},
        .calculation_resolution_context = CalculationResolutionContext::from_computation_context(context),
    };

    if (auto color = to_color(color_resolution_context); color.has_value())
        return create_from_color(*color, ColorSyntax::Modern);

    auto absolutized_color = m_color->absolutized(context);
    if (absolutized_color == m_color)
        return *this;
    return create(move(absolutized_color));
}

bool ContrastColorStyleValue::equals(StyleValue const& other) const
{
    auto const* other_contrast_color = as_if<ContrastColorStyleValue>(other);
    if (!other_contrast_color)
        return false;
    return m_color == other_contrast_color->m_color;
}

void ContrastColorStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    builder.append("contrast-color("sv);
    m_color->serialize(builder, mode);
    builder.append(')');
}

}
