/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ColorStyleValue.h"
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/RGBColorStyleValue.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<ColorStyleValue const> ColorStyleValue::create_from_color(Color color, ColorSyntax color_syntax, Optional<FlyString> const& name)
{
    return RGBColorStyleValue::create(
        NumberStyleValue::create(color.red()),
        NumberStyleValue::create(color.green()),
        NumberStyleValue::create(color.blue()),
        NumberStyleValue::create(color.alpha() / 255.0),
        color_syntax,
        move(name));
}

Optional<double> ColorStyleValue::resolve_hue(StyleValue const& style_value, CalculationResolutionContext const& resolution_context)
{
    // <number> | <angle> | none
    auto normalized = [](double number) {
        // +inf should be clamped to 360
        if (!isfinite(number) && number > 0)
            number = 360.0;

        // -inf and NaN should be clamped to 0
        if (!isfinite(number) || isnan(number))
            number = 0.0;

        return JS::modulo(number, 360.0);
    };

    if (style_value.is_number())
        return normalized(style_value.as_number().number());

    if (style_value.is_angle())
        return normalized(style_value.as_angle().angle().to_degrees());

    if (style_value.is_calculated()) {
        if (style_value.as_calculated().resolves_to_number()) {
            auto maybe_number = style_value.as_calculated().resolve_number(resolution_context);

            if (!maybe_number.has_value())
                return {};

            return normalized(maybe_number.value());
        }

        if (style_value.as_calculated().resolves_to_angle()) {
            auto maybe_angle = style_value.as_calculated().resolve_angle(resolution_context);

            if (!maybe_angle.has_value())
                return {};

            return normalized(maybe_angle.value().to_degrees());
        }
    }

    return 0;
}

Optional<double> ColorStyleValue::resolve_with_reference_value(StyleValue const& style_value, float one_hundred_percent_value, CalculationResolutionContext const& resolution_context)
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
        if (calculated.resolves_to_number()) {
            auto maybe_number = calculated.resolve_number(resolution_context);

            if (!maybe_number.has_value())
                return {};

            return maybe_number.value();
        }

        if (calculated.resolves_to_percentage()) {
            auto percentage = calculated.resolve_percentage(resolution_context);

            if (!percentage.has_value())
                return {};

            return normalize_percentage(percentage.value());
        }
    }

    return 0;
}

Optional<double> ColorStyleValue::resolve_alpha(StyleValue const& style_value, CalculationResolutionContext const& resolution_context)
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
        if (calculated.resolves_to_number()) {
            auto maybe_number = calculated.resolve_number(resolution_context);

            if (!maybe_number.has_value())
                return {};

            return normalized(maybe_number.value());
        }

        if (calculated.resolves_to_percentage()) {
            auto percentage = calculated.resolve_percentage(resolution_context);

            if (!percentage.has_value())
                return {};

            return normalized(percentage.value().as_fraction());
        }
    }

    if (style_value.is_keyword() && style_value.to_keyword() == Keyword::None)
        return 0;

    return 1;
}

void ColorStyleValue::serialize_color_component(StringBuilder& builder, SerializationMode mode, StyleValue const& component, float one_hundred_percent_value, Optional<double> clamp_min, Optional<double> clamp_max) const
{
    if (component.to_keyword() == Keyword::None) {
        builder.append("none"sv);
        return;
    }
    if (component.is_calculated() && mode == SerializationMode::Normal) {
        builder.append(component.to_string(mode));
        return;
    }

    auto maybe_resolved_value = resolve_with_reference_value(component, one_hundred_percent_value, {});

    if (!maybe_resolved_value.has_value()) {
        builder.append(component.to_string(mode));
        return;
    }

    auto resolved_value = maybe_resolved_value.value();

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

void ColorStyleValue::serialize_alpha_component(StringBuilder& builder, SerializationMode mode, StyleValue const& component) const
{
    if (component.to_keyword() == Keyword::None) {
        builder.append("none"sv);
        return;
    }
    if (component.is_calculated() && mode == SerializationMode::Normal) {
        builder.append(component.to_string(mode));
        return;
    }

    auto maybe_resolved_value = resolve_alpha(component, {});

    if (!maybe_resolved_value.has_value()) {
        builder.append(component.to_string(mode));
        return;
    }

    builder.appendff("{}", maybe_resolved_value.value());
}

void ColorStyleValue::serialize_hue_component(StringBuilder& builder, SerializationMode mode, StyleValue const& component) const
{
    if (component.to_keyword() == Keyword::None) {
        builder.append("none"sv);
        return;
    }
    if (component.is_calculated() && mode == SerializationMode::Normal) {
        builder.append(component.to_string(mode));
        return;
    }

    auto maybe_resolved_value = resolve_hue(component, {});

    if (!maybe_resolved_value.has_value()) {
        builder.append(component.to_string(mode));
        return;
    }

    builder.appendff("{:.4}", maybe_resolved_value.value());
}

}
