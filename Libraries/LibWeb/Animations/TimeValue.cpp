/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TimeValue.h"
#include <LibWeb/Animations/AnimationTimeline.h>
#include <LibWeb/CSS/CSSUnitValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>

namespace Web::Animations {

TimeValue TimeValue::from_css_numberish(CSS::CSSNumberish const& time, DOM::AbstractElement const& abstract_element)
{
    if (time.has<double>())
        return { Type::Milliseconds, time.get<double>() };

    auto const& numeric_value = time.get<GC::Root<CSS::CSSNumericValue>>();

    // NB: Skip creating a calculation node for simple unit values
    if (auto const* unit_value = as_if<CSS::CSSUnitValue>(*numeric_value)) {
        if (unit_value->type().matches_number({}))
            return { Type::Milliseconds, unit_value->value() };

        if (unit_value->type().matches_time({}))
            return { Type::Milliseconds, MUST(unit_value->to("ms"_fly_string))->value() };

        if (unit_value->type().matches_percentage())
            return { Type::Percentage, unit_value->value() };

        VERIFY_NOT_REACHED();
    }

    auto const& calculation_node = MUST(numeric_value->create_calculation_node({}));

    VERIFY(calculation_node->numeric_type().has_value());

    auto style_value = CSS::CalculatedStyleValue::create(calculation_node, calculation_node->numeric_type().value(), {});

    CSS::CalculationResolutionContext calculation_resolution_context {
        .length_resolution_context = CSS::Length::ResolutionContext::for_element(abstract_element),
        .abstract_element = abstract_element,
    };

    if (style_value->resolves_to_number())
        return { Type::Milliseconds, style_value->resolve_number(calculation_resolution_context).value() };

    if (style_value->resolves_to_time())
        return { Type::Milliseconds, style_value->resolve_time(calculation_resolution_context)->to_milliseconds() };

    if (style_value->resolves_to_percentage())
        return { Type::Percentage, style_value->resolve_percentage(calculation_resolution_context).value().value() };

    VERIFY_NOT_REACHED();
}

TimeValue TimeValue::create_zero(GC::Ptr<AnimationTimeline> const& timeline)
{
    if (timeline && timeline->is_progress_based())
        return TimeValue { Type::Percentage, 0.0 };

    return TimeValue { Type::Milliseconds, 0.0 };
}

CSS::CSSNumberish TimeValue::as_css_numberish(JS::Realm& realm) const
{
    switch (type) {
    case Type::Milliseconds:
        return value;
    case Type::Percentage:
        GC::Ref<CSS::CSSNumericValue> numeric_value = CSS::CSSUnitValue::create(realm, value, "percent"_fly_string);
        return GC::Root { numeric_value };
    }

    VERIFY_NOT_REACHED();
}

}
