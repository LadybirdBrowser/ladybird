/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ColorMixStyleValue.h"
#include <AK/TypeCasts.h>
#include <LibWeb/CSS/Interpolation.h>
#include <LibWeb/Layout/Node.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<ColorMixStyleValue const> ColorMixStyleValue::create(ColorInterpolationMethod interpolation_method, ColorMixComponent first_component, ColorMixComponent second_component)
{
    return adopt_ref(*new (nothrow) ColorMixStyleValue(move(interpolation_method), move(first_component), move(second_component)));
}

ColorMixStyleValue::ColorMixStyleValue(ColorInterpolationMethod color_interpolation_method, ColorMixComponent first_component, ColorMixComponent second_component)
    : CSSColorValue(ColorType::ColorMix, ColorSyntax::Modern)
    , m_properties {
        .color_interpolation_method = move(color_interpolation_method),
        .first_component = move(first_component),
        .second_component = move(second_component)
    }
{
}

bool ColorMixStyleValue::equals(CSSStyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_color = other.as_color();
    if (color_type() != other_color.color_type())
        return false;
    auto const& other_color_mix = as<ColorMixStyleValue>(other_color);
    return m_properties == other_color_mix.m_properties;
}

// https://drafts.csswg.org/css-color-5/#serial-color-mix
String ColorMixStyleValue::to_string(SerializationMode mode) const
{
    auto serialize_first_percentage = [](StringBuilder& builder, Optional<PercentageOrCalculated> const& p1, Optional<PercentageOrCalculated> const& p2) {
        // if BOTH the first percentage p1 and second percentage p2 are specified:
        if (p1.has_value() && p2.has_value()) {
            // If both p1 equals 50% and p2 equals 50%, nothing is serialized.
            if (!p1->is_calculated() && !p2->is_calculated() && p1->value().value() == 50 && p2->value().value() == 50)
                return;

            // else, p1 is serialized as is.
            builder.appendff(" {}", p1->to_string());
        }
        // else if ONLY the first percentage p1 is specified:
        else if (p1.has_value()) {
            // If p1 is equal to 50%, nothing is serialized.
            if (!p1->is_calculated() && p1->value().value() == 50)
                return;

            // else, p1 is serialized as is.
            builder.appendff(" {}", p1->to_string());
        }
        // else if ONLY the second percentage p2 is specified:
        else if (p2.has_value()) {
            // if p2 equals 50%, nothing is serialized.
            if (!p2->is_calculated() && p2->value().value() == 50)
                return;

            // if p2 is not calc(), the value of 100% - p2 is serialized.
            if (!p2->is_calculated())
                builder.appendff(" {}%", 100 - p2->value().value());

            // else, nothing is serialized.
        }
        // else if NEITHER is specified:
        else {
            // nothing is serialized.
        }
    };

    auto serialize_second_percentage = [](StringBuilder& builder, Optional<PercentageOrCalculated> const& p1, Optional<PercentageOrCalculated> const& p2) {
        // If BOTH the first percentage p1 and second percentages p2 are specified:
        if (p1.has_value() && p2.has_value()) {
            // if neither p1 nor p2 is calc(), and p1 + p2 equals 100%, nothing is serialized.
            if (!p1->is_calculated() && !p2->is_calculated() && p1->value().value() + p2->value().value() == 100)
                return;

            // else, p2 is serialized as is.
            builder.appendff(" {}", p2->to_string());
        }
        // else if ONLY the first percentage p1 is specified:
        else if (p1.has_value()) {
            // nothing is serialized.
        }
        // else if ONLY the second percentage p2 is specified:
        else if (p2.has_value()) {
            // if p2 equals 50%, nothing is serialized.
            if (!p2->is_calculated() && p2->value().value() == 50)
                return;

            // if p2 is not calc(), nothing is serialized.
            if (!p2->is_calculated())
                return;

            // else, p2 is serialized as is.
            builder.appendff(" {}", p2->to_string());
        }
        // else if NEITHER is specified:
        else {
            // nothing is serialized.
        }
    };

    StringBuilder builder;
    builder.appendff("color-mix(in {}", m_properties.color_interpolation_method.color_space);
    if (m_properties.color_interpolation_method.hue_interpolation_method.value_or(HueInterpolationMethod::Shorter) != HueInterpolationMethod::Shorter)
        builder.appendff(" {} hue", CSS::to_string(*m_properties.color_interpolation_method.hue_interpolation_method));
    builder.append(", "sv);
    builder.append(m_properties.first_component.color->to_string(mode));
    serialize_first_percentage(builder, m_properties.first_component.percentage, m_properties.second_component.percentage);
    builder.appendff(", {}", m_properties.second_component.color->to_string(mode));
    serialize_second_percentage(builder, m_properties.first_component.percentage, m_properties.second_component.percentage);
    builder.append(')');
    return MUST(builder.to_string());
}

// https://drafts.csswg.org/css-color-5/#color-mix-percent-norm
ColorMixStyleValue::PercentageNormalizationResult ColorMixStyleValue::normalize_percentages() const
{
    auto resolve_percentage = [&](Optional<PercentageOrCalculated> const& percentage_or_calculated) -> Optional<Percentage> {
        if (!percentage_or_calculated.has_value())
            return {};
        if (!percentage_or_calculated->is_calculated())
            return percentage_or_calculated->value();
        return percentage_or_calculated->resolved({});
    };

    // 1. Let p1 be the first percentage and p2 the second one.
    auto p1 = resolve_percentage(m_properties.first_component.percentage);
    auto p2 = resolve_percentage(m_properties.second_component.percentage);
    double alpha_multiplier = 0;

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

    return PercentageNormalizationResult { .p1 = *p1, .p2 = *p2, .alpha_multiplier = alpha_multiplier };
}

// https://drafts.csswg.org/css-color-5/#color-mix-result
Color ColorMixStyleValue::to_color(Optional<Layout::NodeWithStyle const&> node) const
{
    // FIXME: Do this in a spec-compliant way.
    //        Our color interpolation doesn't currently take the color space or hue interpolation method into account.
    auto normalized_percentages = normalize_percentages();
    auto from_color = m_properties.first_component.color->to_color(node);
    auto to_color = m_properties.second_component.color->to_color(node);
    auto delta = normalized_percentages.p2.value() / 100;
    return interpolate_color(from_color, to_color, delta);
}

}
