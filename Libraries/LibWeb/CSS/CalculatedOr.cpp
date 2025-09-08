/*
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CalculatedOr.h"
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/FlexStyleValue.h>
#include <LibWeb/CSS/StyleValues/FrequencyStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/ResolutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>

namespace Web::CSS {

Optional<Angle> AngleOrCalculated::resolve_calculated(NonnullRefPtr<CalculatedStyleValue const> const& calculated, CalculationResolutionContext const& context) const
{
    return calculated->resolve_angle_deprecated(context);
}

NonnullRefPtr<StyleValue const> AngleOrCalculated::create_style_value() const
{
    return AngleStyleValue::create(value());
}

Optional<Flex> FlexOrCalculated::resolve_calculated(NonnullRefPtr<CalculatedStyleValue const> const& calculated, CalculationResolutionContext const& context) const
{
    return calculated->resolve_flex_deprecated(context);
}

NonnullRefPtr<StyleValue const> FlexOrCalculated::create_style_value() const
{
    return FlexStyleValue::create(value());
}

Optional<Frequency> FrequencyOrCalculated::resolve_calculated(NonnullRefPtr<CalculatedStyleValue const> const& calculated, CalculationResolutionContext const& context) const
{
    return calculated->resolve_frequency_deprecated(context);
}

NonnullRefPtr<StyleValue const> FrequencyOrCalculated::create_style_value() const
{
    return FrequencyStyleValue::create(value());
}

Optional<i64> IntegerOrCalculated::resolve_calculated(NonnullRefPtr<CalculatedStyleValue const> const& calculated, CalculationResolutionContext const& context) const
{
    return calculated->resolve_integer_deprecated(context);
}

NonnullRefPtr<StyleValue const> IntegerOrCalculated::create_style_value() const
{
    return IntegerStyleValue::create(value());
}

Optional<Length> LengthOrCalculated::resolve_calculated(NonnullRefPtr<CalculatedStyleValue const> const& calculated, CalculationResolutionContext const& context) const
{
    return calculated->resolve_length_deprecated(context);
}

NonnullRefPtr<StyleValue const> LengthOrCalculated::create_style_value() const
{
    return LengthStyleValue::create(value());
}

Optional<LengthOrAuto> LengthOrAutoOrCalculated::resolve_calculated(NonnullRefPtr<CalculatedStyleValue const> const& calculated, CalculationResolutionContext const& context) const
{
    return calculated->resolve_length(context).map([](auto& length) { return LengthOrAuto { length }; });
}

NonnullRefPtr<StyleValue const> LengthOrAutoOrCalculated::create_style_value() const
{
    auto const& length_or_auto = value();
    if (length_or_auto.is_auto())
        return KeywordStyleValue::create(Keyword::Auto);
    return LengthStyleValue::create(length_or_auto.length());
}

bool LengthOrAutoOrCalculated::is_auto() const
{
    return !is_calculated() && value().is_auto();
}

LengthOrCalculated LengthOrAutoOrCalculated::without_auto() const
{
    VERIFY(!is_auto());
    if (is_calculated())
        return calculated();
    return value().length();
}

Optional<double> NumberOrCalculated::resolve_calculated(NonnullRefPtr<CalculatedStyleValue const> const& calculated, CalculationResolutionContext const& context) const
{
    return calculated->resolve_number_deprecated(context);
}

NonnullRefPtr<StyleValue const> NumberOrCalculated::create_style_value() const
{
    return NumberStyleValue::create(value());
}

Optional<Percentage> PercentageOrCalculated::resolve_calculated(NonnullRefPtr<CalculatedStyleValue const> const& calculated, CalculationResolutionContext const& context) const
{
    return calculated->resolve_percentage_deprecated(context);
}

NonnullRefPtr<StyleValue const> PercentageOrCalculated::create_style_value() const
{
    return PercentageStyleValue::create(value());
}

Optional<Resolution> ResolutionOrCalculated::resolve_calculated(NonnullRefPtr<CalculatedStyleValue const> const& calculated, CalculationResolutionContext const& context) const
{
    return calculated->resolve_resolution_deprecated(context);
}

NonnullRefPtr<StyleValue const> ResolutionOrCalculated::create_style_value() const
{
    return ResolutionStyleValue::create(value());
}

Optional<Time> TimeOrCalculated::resolve_calculated(NonnullRefPtr<CalculatedStyleValue const> const& calculated, CalculationResolutionContext const& context) const
{
    return calculated->resolve_time_deprecated(context);
}

NonnullRefPtr<StyleValue const> TimeOrCalculated::create_style_value() const
{
    return TimeStyleValue::create(value());
}

}
