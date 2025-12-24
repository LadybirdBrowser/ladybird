/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "EdgeStyleValue.h"
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/ValueType.h>

namespace Web::CSS {

bool EdgeStyleValue::is_center() const
{
    if (m_properties.edge == PositionEdge::Center)
        return true;

    if (m_properties.offset && m_properties.offset->is_percentage() && m_properties.offset->as_percentage().percentage().value() == 50)
        return true;

    return false;
}

String EdgeStyleValue::to_string(SerializationMode mode) const
{
    StringBuilder builder;

    if (m_properties.edge.has_value())
        builder.append(CSS::to_string(m_properties.edge.value()));

    if (m_properties.edge.has_value() && m_properties.offset)
        builder.append(' ');

    if (m_properties.offset)
        builder.append(m_properties.offset->to_string(mode));

    return builder.to_string_without_validation();
}

ValueComparingNonnullRefPtr<StyleValue const> EdgeStyleValue::absolutized(ComputationContext const& computation_context) const
{
    if (m_properties.edge == PositionEdge::Center)
        return EdgeStyleValue::create({}, PercentageStyleValue::create(Percentage(50)));

    CalculationContext calculation_context {
        .percentages_resolve_as = ValueType::Length,
    };

    if (m_properties.edge == PositionEdge::Right || m_properties.edge == PositionEdge::Bottom) {
        if (!m_properties.offset)
            return EdgeStyleValue::create({}, PercentageStyleValue::create(Percentage(100)));

        auto flipped_percentage = SumCalculationNode::create({ NumericCalculationNode::create(Percentage { 100 }, calculation_context),
            NegateCalculationNode::create(CalculationNode::from_style_value(*m_properties.offset, calculation_context)) });

        auto flipped_percentage_style_value = CalculatedStyleValue::create(flipped_percentage, NumericType(NumericType::BaseType::Length, 1), calculation_context);

        return EdgeStyleValue::create({}, flipped_percentage_style_value->absolutized(computation_context));
    }

    if (!m_properties.offset)
        return EdgeStyleValue::create({}, PercentageStyleValue::create(Percentage(0)));

    return EdgeStyleValue::create({}, m_properties.offset->absolutized(computation_context));
}

}
