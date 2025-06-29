/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSColorValue.h"
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/CSSRGB.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<CSSColorValue const> CSSColorValue::create_from_color(Color color, ColorSyntax color_syntax, Optional<FlyString> name)
{
    return CSSRGB::create(
        NumberStyleValue::create(color.red()),
        NumberStyleValue::create(color.green()),
        NumberStyleValue::create(color.blue()),
        NumberStyleValue::create(color.alpha() / 255.0),
        color_syntax,
        name);
}

Optional<double> CSSColorValue::resolve_hue(CSSStyleValue const& style_value, CalculationResolutionContext const& resolution_context)
{
    // <number> | <angle> | none
    auto normalized = [](double number) {
        return JS::modulo(number, 360.0);
    };

    if (style_value.is_number())
        return normalized(style_value.as_number().number());

    if (style_value.is_angle())
        return normalized(style_value.as_angle().angle().to_degrees());

    if (style_value.is_calculated() && style_value.as_calculated().resolves_to_angle())
        return normalized(style_value.as_calculated().resolve_angle(resolution_context).value().to_degrees());

    if (style_value.is_keyword() && style_value.to_keyword() == Keyword::None)
        return 0;

    return {};
}

Optional<double> CSSColorValue::resolve_with_reference_value(CSSStyleValue const& style_value, float one_hundred_percent_value, CalculationResolutionContext const& resolution_context)
{
    // <percentage> | <number> | none
    auto normalize_percentage = [one_hundred_percent_value](Percentage const& percentage) {
        return percentage.as_fraction() * one_hundred_percent_value;
    };

    if (style_value.is_percentage())
        return normalize_percentage(style_value.as_percentage().percentage());

    if (style_value.is_number())
        return style_value.as_number().number();

    if (style_value.is_calculated()) {
        auto const& calculated = style_value.as_calculated();
        if (calculated.resolves_to_number())
            return calculated.resolve_number(resolution_context).value();
        if (calculated.resolves_to_percentage())
            return normalize_percentage(calculated.resolve_percentage(resolution_context).value());
    }

    if (style_value.is_keyword() && style_value.to_keyword() == Keyword::None)
        return 0;

    return {};
}

Optional<double> CSSColorValue::resolve_alpha(CSSStyleValue const& style_value, CalculationResolutionContext const& resolution_context)
{
    // <number> | <percentage> | none
    auto normalized = [](double number) {
        if (isnan(number))
            number = 0;
        return clamp(number, 0.0, 1.0);
    };

    if (style_value.is_number())
        return normalized(style_value.as_number().number());

    if (style_value.is_percentage())
        return normalized(style_value.as_percentage().percentage().as_fraction());

    if (style_value.is_calculated()) {
        auto const& calculated = style_value.as_calculated();
        if (calculated.resolves_to_number())
            return normalized(calculated.resolve_number(resolution_context).value());
        if (calculated.resolves_to_percentage())
            return normalized(calculated.resolve_percentage(resolution_context).value().as_fraction());
    }

    if (style_value.is_keyword() && style_value.to_keyword() == Keyword::None)
        return 0;

    return {};
}

void CSSColorValue::serialize_color_component(StringBuilder& builder, SerializationMode mode, CSSStyleValue const& component, float one_hundred_percent_value, Optional<double> clamp_min, Optional<double> clamp_max) const
{
    if (component.to_keyword() == Keyword::None) {
        builder.append("none"sv);
        return;
    }
    if (component.is_calculated() && mode == SerializationMode::Normal) {
        builder.append(component.to_string(mode));
        return;
    }
    auto resolved_value = resolve_with_reference_value(component, one_hundred_percent_value, {}).value_or(0);
    if (clamp_min.has_value() && resolved_value < *clamp_min)
        resolved_value = *clamp_min;
    if (clamp_max.has_value() && resolved_value > *clamp_max)
        resolved_value = *clamp_max;

    // FIXME: Find a better way to format a decimal with trimmed trailing zeroes
    auto resolved_string = MUST(String::formatted("{:.2}", resolved_value));
    if (resolved_string.contains('.'))
        resolved_string = MUST(resolved_string.trim("0"sv, TrimMode::Right));
    builder.append(resolved_string);
}

void CSSColorValue::serialize_alpha_component(StringBuilder& builder, SerializationMode mode, CSSStyleValue const& component) const
{
    if (component.to_keyword() == Keyword::None) {
        builder.append("none"sv);
        return;
    }
    if (component.is_calculated() && mode == SerializationMode::Normal) {
        builder.append(component.to_string(mode));
        return;
    }
    auto resolved_value = resolve_alpha(component, {}).value_or(0);
    builder.appendff("{}", resolved_value);
}

void CSSColorValue::serialize_hue_component(StringBuilder& builder, SerializationMode mode, CSSStyleValue const& component) const
{
    if (component.to_keyword() == Keyword::None) {
        builder.append("none"sv);
        return;
    }
    if (component.is_calculated() && mode == SerializationMode::Normal) {
        builder.append(component.to_string(mode));
        return;
    }
    builder.appendff("{:.4}", resolve_hue(component, {}).value_or(0));
}

}
