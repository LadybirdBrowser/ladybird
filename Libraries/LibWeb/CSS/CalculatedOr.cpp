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
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/ResolutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>

namespace Web::CSS {

Optional<Angle> AngleOrCalculated::resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const& calculated, CalculationResolutionContext const& context) const
{
    return calculated->resolve_angle(context);
}

NonnullRefPtr<CSSStyleValue> AngleOrCalculated::create_style_value() const
{
    return AngleStyleValue::create(value());
}

Optional<Flex> FlexOrCalculated::resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const& calculated, CalculationResolutionContext const& context) const
{
    return calculated->resolve_flex(context);
}

NonnullRefPtr<CSSStyleValue> FlexOrCalculated::create_style_value() const
{
    return FlexStyleValue::create(value());
}

Optional<Frequency> FrequencyOrCalculated::resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const& calculated, CalculationResolutionContext const& context) const
{
    return calculated->resolve_frequency(context);
}

NonnullRefPtr<CSSStyleValue> FrequencyOrCalculated::create_style_value() const
{
    return FrequencyStyleValue::create(value());
}

Optional<i64> IntegerOrCalculated::resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const& calculated, CalculationResolutionContext const& context) const
{
    return calculated->resolve_integer(context);
}

NonnullRefPtr<CSSStyleValue> IntegerOrCalculated::create_style_value() const
{
    return IntegerStyleValue::create(value());
}

Optional<Length> LengthOrCalculated::resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const& calculated, CalculationResolutionContext const& context) const
{
    return calculated->resolve_length(context);
}

NonnullRefPtr<CSSStyleValue> LengthOrCalculated::create_style_value() const
{
    return LengthStyleValue::create(value());
}

Optional<double> NumberOrCalculated::resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const& calculated, CalculationResolutionContext const& context) const
{
    return calculated->resolve_number(context);
}

NonnullRefPtr<CSSStyleValue> NumberOrCalculated::create_style_value() const
{
    return NumberStyleValue::create(value());
}

Optional<Percentage> PercentageOrCalculated::resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const& calculated, CalculationResolutionContext const& context) const
{
    return calculated->resolve_percentage(context);
}

NonnullRefPtr<CSSStyleValue> PercentageOrCalculated::create_style_value() const
{
    return PercentageStyleValue::create(value());
}

Optional<Resolution> ResolutionOrCalculated::resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const& calculated, CalculationResolutionContext const& context) const
{
    return calculated->resolve_resolution(context);
}

NonnullRefPtr<CSSStyleValue> ResolutionOrCalculated::create_style_value() const
{
    return ResolutionStyleValue::create(value());
}

Optional<Time> TimeOrCalculated::resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const& calculated, CalculationResolutionContext const& context) const
{
    return calculated->resolve_time(context);
}

NonnullRefPtr<CSSStyleValue> TimeOrCalculated::create_style_value() const
{
    return TimeStyleValue::create(value());
}

}
