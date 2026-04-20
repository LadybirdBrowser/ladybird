/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ColorMixStyleValue.h"
#include <AK/TypeCasts.h>
#include <LibWeb/CSS/ColorInterpolation.h>
#include <LibWeb/CSS/Interpolation.h>
#include <LibWeb/CSS/StyleValues/ColorInterpolationMethodStyleValue.h>
#include <LibWeb/Layout/Node.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<ColorMixStyleValue const> ColorMixStyleValue::create(RefPtr<StyleValue const> color_interpolation_method, ColorMixComponent first_component, ColorMixComponent second_component)
{
    return adopt_ref(*new (nothrow) ColorMixStyleValue(move(color_interpolation_method), move(first_component), move(second_component)));
}

ColorMixStyleValue::ColorMixStyleValue(RefPtr<StyleValue const> color_interpolation_method, ColorMixComponent first_component, ColorMixComponent second_component)
    : ColorStyleValue({}, ColorSyntax::Modern)
    , m_properties {
        .color_interpolation_method = move(color_interpolation_method),
        .first_component = move(first_component),
        .second_component = move(second_component)
    }
{
}

bool ColorMixStyleValue::equals(StyleValue const& other) const
{
    auto const* other_color_mix = as_if<ColorMixStyleValue>(other);
    if (!other_color_mix)
        return false;
    return m_properties == other_color_mix->m_properties;
}

// https://drafts.csswg.org/css-color-5/#serial-color-mix
void ColorMixStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    auto serialize_first_percentage = [&mode](StringBuilder& builder, RefPtr<StyleValue const> const& p1, RefPtr<StyleValue const> const& p2) {
        // if BOTH the first percentage p1 and second percentage p2 are specified:
        if (p1 && p2) {
            // If both p1 equals 50% and p2 equals 50%, nothing is serialized.
            if (p1->is_percentage() && p2->is_percentage() && p1->as_percentage().percentage().value() == 50 && p2->as_percentage().percentage().value() == 50)
                return;

            // else, p1 is serialized as is.
            builder.append(' ');
            p1->serialize(builder, mode);
        }
        // else if ONLY the first percentage p1 is specified:
        else if (p1) {
            // If p1 is equal to 50%, nothing is serialized.
            if (p1->is_percentage() && p1->as_percentage().percentage().value() == 50)
                return;

            // else, p1 is serialized as is.
            builder.append(' ');
            p1->serialize(builder, mode);
        }
        // else if ONLY the second percentage p2 is specified:
        else if (p2) {
            // if p2 equals 50%, nothing is serialized.
            if (p2->is_percentage() && p2->as_percentage().percentage().value() == 50)
                return;

            // if p2 is not calc(), the value of 100% - p2 is serialized.
            if (!p2->is_calculated())
                builder.appendff(" {}%", 100 - p2->as_percentage().percentage().value());

            // else, nothing is serialized.
        }
        // else if NEITHER is specified:
        else {
            // nothing is serialized.
        }
    };

    auto serialize_second_percentage = [&mode](StringBuilder& builder, RefPtr<StyleValue const> const& p1, RefPtr<StyleValue const> const& p2) {
        // If BOTH the first percentage p1 and second percentages p2 are specified:
        if (p1 && p2) {
            // if neither p1 nor p2 is calc(), and p1 + p2 equals 100%, nothing is serialized.
            if (p1->is_percentage() && p2->is_percentage() && p1->as_percentage().percentage().value() + p2->as_percentage().percentage().value() == 100)
                return;

            // else, p2 is serialized as is.
            builder.append(' ');
            p2->serialize(builder, mode);
        }
        // else if ONLY the first percentage p1 is specified:
        else if (p1) {
            // nothing is serialized.
        }
        // else if ONLY the second percentage p2 is specified:
        else if (p2) {
            // if p2 equals 50%, nothing is serialized.
            if (p2->is_percentage() && p2->as_percentage().percentage().value() == 50)
                return;

            // if p2 is not calc(), nothing is serialized.
            if (!p2->is_calculated())
                return;

            // else, p2 is serialized as is.
            builder.append(' ');
            p2->serialize(builder, mode);
        }
        // else if NEITHER is specified:
        else {
            // nothing is serialized.
        }
    };

    builder.append("color-mix("sv);

    if (m_properties.color_interpolation_method && m_properties.color_interpolation_method->as_color_interpolation_method().color_interpolation_method() != RectangularColorSpace::Oklab) {
        m_properties.color_interpolation_method->serialize(builder, mode);
        builder.append(", "sv);
    }

    m_properties.first_component.color->serialize(builder, mode);
    serialize_first_percentage(builder, m_properties.first_component.percentage, m_properties.second_component.percentage);
    builder.append(", "sv);
    m_properties.second_component.color->serialize(builder, mode);
    serialize_second_percentage(builder, m_properties.first_component.percentage, m_properties.second_component.percentage);
    builder.append(')');
}

// https://drafts.csswg.org/css-color-5/#color-mix-percent-norm
ColorMixStyleValue::NormalizedPercentages ColorMixStyleValue::normalize_percentage_pair(Optional<Percentage> p1, Optional<Percentage> p2)
{
    double alpha_multiplier = 1.0;

    // 1. Let p1 be the first percentage and p2 the second one.
    // NB: Provided by the caller.

    // 2. If both percentages are omitted, they each default to 50% (an equal mix of the two colors).
    if (!p1.has_value() && !p2.has_value()) {
        p1 = Percentage(50);
        p2 = Percentage(50);
    }
    // 3. Otherwise, if p2 is omitted, it becomes 100% - p1
    else if (!p2.has_value()) {
        p2 = Percentage(100 - p1->value());
    }
    // 4. Otherwise, if p1 is omitted, it becomes 100% - p2
    else if (!p1.has_value()) {
        p1 = Percentage(100 - p2->value());
    }
    // 5. Otherwise, if both are provided and add up to greater than 100%, they are scaled accordingly so that they add up to 100%.
    else if (p1->value() + p2->value() > 100) {
        auto sum = p1->value() + p2->value();
        p1 = Percentage((p1->value() / sum) * 100);
        p2 = Percentage((p2->value() / sum) * 100);
    }
    // 6. Otherwise, if both are provided and add up to less than 100%, the sum is saved as an alpha multiplier. If the sum is greater than zero, they are then scaled accordingly so that they add up to 100%.
    else if (p1->value() + p2->value() < 100) {
        auto sum = p1->value() + p2->value();
        alpha_multiplier = sum / 100;
        if (sum > 0) {
            p1 = Percentage((p1->value() / sum) * 100);
            p2 = Percentage((p2->value() / sum) * 100);
        }
    }

    VERIFY(p1.has_value());
    VERIFY(p2.has_value());

    return { .first_percentage = *p1, .second_percentage = *p2, .alpha_multiplier = alpha_multiplier };
}

ColorMixStyleValue::PercentageNormalizationResult ColorMixStyleValue::normalize_percentages(ComputationContext const& computation_context) const
{
    auto p1 = m_properties.first_component.percentage
        ? Percentage::from_style_value(m_properties.first_component.percentage->absolutized(computation_context))
        : Optional<Percentage> {};
    auto p2 = m_properties.second_component.percentage
        ? Percentage::from_style_value(m_properties.second_component.percentage->absolutized(computation_context))
        : Optional<Percentage> {};

    auto result = normalize_percentage_pair(p1, p2);
    return {
        .p1 = PercentageStyleValue::create(result.first_percentage),
        .p2 = PercentageStyleValue::create(result.second_percentage),
        .alpha_multiplier = result.alpha_multiplier,
    };
}

// https://drafts.csswg.org/css-color-5/#color-mix-result
Optional<Color> ColorMixStyleValue::to_color(ColorResolutionContext color_resolution_context) const
{
    auto p1 = m_properties.first_component.percentage
        ? Optional<Percentage>(Percentage::from_style_value(*m_properties.first_component.percentage))
        : Optional<Percentage> {};
    auto p2 = m_properties.second_component.percentage
        ? Optional<Percentage>(Percentage::from_style_value(*m_properties.second_component.percentage))
        : Optional<Percentage> {};
    auto normalized = normalize_percentage_pair(p1, p2);

    auto color_interpolation_method = m_properties.color_interpolation_method
        ? m_properties.color_interpolation_method->as_color_interpolation_method().color_interpolation_method()
        : ColorInterpolationMethodStyleValue::ColorInterpolationMethod { RectangularColorSpace::Oklab };

    auto interpolated = perform_color_interpolation(*m_properties.first_component.color, *m_properties.second_component.color, normalized.second_percentage.as_fraction(), color_interpolation_method, color_resolution_context);
    if (!interpolated.has_value())
        return {};

    if (normalized.alpha_multiplier < 1.0)
        interpolated->components.set_alpha(interpolated->components.alpha() * normalized.alpha_multiplier);

    auto style_value = style_value_for_interpolated_color(*interpolated);
    if (!style_value)
        return {};

    return style_value->to_color(color_resolution_context);
}

ValueComparingNonnullRefPtr<StyleValue const> ColorMixStyleValue::absolutized(ComputationContext const& context) const
{
    // FIXME: Follow the spec algorithm. https://drafts.csswg.org/css-color-5/#calculate-a-color-mix

    auto normalized_percentages = normalize_percentages(context);
    ColorResolutionContext color_resolution_context {
        .color_scheme = context.color_scheme,
        .current_color = {},
        .accent_color = {},
        .document = context.abstract_element.map([](auto& it) { return &it.document(); }).value_or(nullptr),
        .calculation_resolution_context = CalculationResolutionContext::from_computation_context(context),
    };
    auto absolutized_color_interpolation_method = m_properties.color_interpolation_method ? ValueComparingRefPtr<StyleValue const> { m_properties.color_interpolation_method->absolutized(context) } : nullptr;

    auto delta = Percentage::from_style_value(normalized_percentages.p2).as_fraction();

    auto color_interpolation_method = absolutized_color_interpolation_method
        ? absolutized_color_interpolation_method->as_color_interpolation_method().color_interpolation_method()
        : ColorInterpolationMethodStyleValue::ColorInterpolationMethod { RectangularColorSpace::Oklab };

    if (auto interpolated = perform_color_interpolation(*m_properties.first_component.color, *m_properties.second_component.color, delta, color_interpolation_method, color_resolution_context); interpolated.has_value()) {
        if (normalized_percentages.alpha_multiplier < 1.0)
            interpolated->components.set_alpha(interpolated->components.alpha() * normalized_percentages.alpha_multiplier);
        if (auto style_value = style_value_for_interpolated_color(*interpolated))
            return style_value.release_nonnull();
    }

    // Fall back to returning a color-mix() with absolutized values if we can't compute completely.
    // Currently, this is only the case if one of our colors relies on `currentcolor`, as that does not compute to a color value.
    auto absolutized_first_color = m_properties.first_component.color->absolutized(context);
    auto absolutized_second_color = m_properties.second_component.color->absolutized(context);
    if (absolutized_first_color == m_properties.first_component.color && normalized_percentages.p1 == m_properties.first_component.percentage
        && absolutized_second_color == m_properties.second_component.color && normalized_percentages.p2 == m_properties.second_component.percentage)
        return *this;

    return ColorMixStyleValue::create(
        absolutized_color_interpolation_method,
        ColorMixComponent { .color = move(absolutized_first_color), .percentage = normalized_percentages.p1 },
        ColorMixComponent { .color = move(absolutized_second_color), .percentage = normalized_percentages.p2 });
}

}
