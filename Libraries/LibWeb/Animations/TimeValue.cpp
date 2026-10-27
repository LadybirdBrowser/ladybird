/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TimeValue.h"
#include <LibWeb/Animations/AnimationTimeline.h>
#include <LibWeb/CSS/CSSUnitValue.h>
#include <LibWeb/CSS/CalculationResolutionContext.h>
#include <LibWeb/CSS/StyleValues/CalcNodeRef.h>
#include <LibWeb/CSS/Units.h>

namespace Web::Animations {

TimeValue TimeValue::from_css_numberish(CSS::CSSNumberish const& time, CSS::ComputationContext const& computation_context)
{
    if (time.has<double>())
        return { Type::Milliseconds, time.get<double>() };

    auto const& numeric_value = time.get<GC::Ref<CSS::CSSNumericValue>>();

    // NB: Skip creating a calculation node for simple unit values
    if (auto const* unit_value = as_if<CSS::CSSUnitValue>(*numeric_value)) {
        if (unit_value->type().matches_number({}))
            return { Type::Milliseconds, unit_value->value() };

        if (unit_value->type().matches_time({}))
            return { Type::Milliseconds, MUST(unit_value->to("ms"_utf16_fly_string))->value() };

        if (unit_value->type().matches_percentage())
            return { Type::Percentage, unit_value->value() };

        VERIFY_NOT_REACHED();
    }

    auto calculation_node = MUST(numeric_value->create_calculation_node({}));

    auto numeric_type = calculation_node.determine_type({});
    VERIFY(numeric_type.has_value());

    auto style_value = CSS::create_calculated_style_value(move(calculation_node), numeric_type.release_value(), {});

    auto calculation_resolution_context = CSS::CalculationResolutionContext::from_computation_context(computation_context);

    using CSS::StyleValueFFI::CanonicalNumericType;

    if (CSS::calculation_resolves_to(*style_value, CanonicalNumericType::Number))
        return { Type::Milliseconds, CSS::resolve_numeric_style_value(*style_value, CanonicalNumericType::Number, calculation_resolution_context).value() };

    if (CSS::calculation_resolves_to(*style_value, CanonicalNumericType::Time))
        return { Type::Milliseconds, CSS::ratio_between_units(CSS::TimeUnit::S, CSS::TimeUnit::Ms) * CSS::resolve_numeric_style_value(*style_value, CanonicalNumericType::Time, calculation_resolution_context).value() };

    if (CSS::calculation_resolves_to(*style_value, CanonicalNumericType::Percentage))
        return { Type::Percentage, CSS::resolve_numeric_style_value(*style_value, CanonicalNumericType::Percentage, calculation_resolution_context).value() };

    VERIFY_NOT_REACHED();
}

TimeValue TimeValue::create_zero(GC::Ptr<AnimationTimeline> const& timeline)
{
    if (timeline && timeline->is_progress_based())
        return TimeValue { Type::Percentage, 0.0 };

    return TimeValue { Type::Milliseconds, 0.0 };
}

CSS::CSSNumberish TimeValue::as_css_numberish() const
{
    switch (type) {
    case Type::Milliseconds:
        return value;
    case Type::Percentage:
        GC::Ref<CSS::CSSNumericValue> numeric_value = CSS::CSSUnitValue::create(value, "percent"_utf16_fly_string);
        return numeric_value;
    }

    VERIFY_NOT_REACHED();
}

}
