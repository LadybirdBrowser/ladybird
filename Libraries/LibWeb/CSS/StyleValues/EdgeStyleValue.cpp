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

bool EdgeStyleValue::is_center(SerializationMode mode) const
{
    if (m_properties.edge == PositionEdge::Center)
        return true;

    if (m_properties.offset && m_properties.offset->to_string(mode) == "50%"sv)
        return true;

    return false;
}

void EdgeStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    if (m_properties.edge.has_value())
        builder.append(CSS::to_string(m_properties.edge.value()));

    if (m_properties.edge.has_value() && m_properties.offset)
        builder.append(' ');

    if (m_properties.offset)
        m_properties.offset->serialize(builder, mode);
}

ValueComparingNonnullRefPtr<EdgeStyleValue const> EdgeStyleValue::with_resolved_keywords() const
{
    if (m_properties.edge == PositionEdge::Center)
        return EdgeStyleValue::create({}, PercentageStyleValue::create(Percentage(50)));

    CalculationContext calculation_context {
        .percentages_resolve_as = ValueType::Length,
    };

    if (m_properties.edge == PositionEdge::Right || m_properties.edge == PositionEdge::Bottom) {
        if (!m_properties.offset)
            return EdgeStyleValue::create({}, PercentageStyleValue::create(Percentage(100)));

        auto negated_offset = NegateCalculationNode::create(CalculationNode::from_style_value(*m_properties.offset, calculation_context));

        auto flipped_offset = simplify_a_calculation_tree(
            SumCalculationNode::create({ NumericCalculationNode::create(Percentage { 100 }, calculation_context), negated_offset }),
            calculation_context,
            {});

        auto flipped_percentage_style_value = CalculatedStyleValue::create(flipped_offset, NumericType(NumericType::BaseType::Length, 1), calculation_context);

        return EdgeStyleValue::create({}, flipped_percentage_style_value);
    }

    if (!m_properties.offset)
        return EdgeStyleValue::create({}, PercentageStyleValue::create(Percentage(0)));

    return EdgeStyleValue::create({}, m_properties.offset);
}

ValueComparingNonnullRefPtr<StyleValue const> EdgeStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto absolutized_offset = with_resolved_keywords()->offset()->absolutized(computation_context);
    if (!m_properties.edge.has_value() && m_properties.offset == absolutized_offset)
        return *this;
    return EdgeStyleValue::create({}, absolutized_offset);
}

}
