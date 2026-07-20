/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "EdgeStyleValue.h"
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/CalcNodeRef.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/ValueType.h>

namespace Web::CSS {

bool EdgeStyleValue::is_center(SerializationMode mode) const
{
    if (edge() == PositionEdge::Center)
        return true;

    if (offset_style_value() && offset_style_value()->to_string(mode) == "50%"sv)
        return true;

    return false;
}

void EdgeStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    if (edge().has_value())
        builder.append(CSS::to_string(edge().value()));

    if (edge().has_value() && offset_style_value())
        builder.append(' ');

    if (offset_style_value())
        offset_style_value()->serialize(builder, mode);
}

ValueComparingNonnullRefPtr<EdgeStyleValue const> EdgeStyleValue::with_resolved_keywords() const
{
    if (edge() == PositionEdge::Center)
        return EdgeStyleValue::create({}, PercentageStyleValue::create(Percentage(50)));

    CalculationContext calculation_context {
        .percentages_resolve_as = ValueType::Length,
    };

    if (edge() == PositionEdge::Right || edge() == PositionEdge::Bottom) {
        if (!offset_style_value())
            return EdgeStyleValue::create({}, PercentageStyleValue::create(Percentage(100)));

        auto negated_offset = CalcNodeRef::negate(CalcNodeRef::from_style_value(*offset_style_value()));

        Vector<CalcNodeRef> sum_components;
        sum_components.append(CalcNodeRef::numeric(Percentage { 100 }));
        sum_components.append(move(negated_offset));
        auto flipped_offset = simplify_a_calculation_tree(CalcNodeRef::sum(move(sum_components)), calculation_context, {});

        auto flipped_percentage_style_value = CalculatedStyleValue::create(move(flipped_offset), NumericType(NumericType::BaseType::Length, 1), calculation_context);

        return EdgeStyleValue::create({}, flipped_percentage_style_value);
    }

    if (!offset_style_value())
        return EdgeStyleValue::create({}, PercentageStyleValue::create(Percentage(0)));

    return EdgeStyleValue::create({}, offset_style_value());
}

ValueComparingNonnullRefPtr<StyleValue const> EdgeStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto absolutized_offset = with_resolved_keywords()->offset()->absolutized(computation_context);
    if (!edge().has_value() && offset_style_value() == absolutized_offset)
        return *this;
    return EdgeStyleValue::create({}, absolutized_offset);
}

}
