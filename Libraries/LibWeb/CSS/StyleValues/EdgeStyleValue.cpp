/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "EdgeStyleValue.h"
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>

namespace Web::CSS {

String EdgeStyleValue::to_string(SerializationMode mode) const
{
    if (mode == SerializationMode::ResolvedValue) {
        // FIXME: Figure out how to get the proper calculation context here
        CalculationContext context {};
        return resolved_value(context)->offset()->to_string(mode);
    }

    StringBuilder builder;

    if (m_properties.edge.has_value())
        builder.append(CSS::to_string(m_properties.edge.value()));

    if (m_properties.edge.has_value() && m_properties.offset)
        builder.append(' ');

    if (m_properties.offset)
        builder.append(m_properties.offset->to_string(mode));

    return builder.to_string_without_validation();
}

ValueComparingNonnullRefPtr<EdgeStyleValue const> EdgeStyleValue::resolved_value(CalculationContext context) const
{
    if (edge() == PositionEdge::Right || edge() == PositionEdge::Bottom) {
        if (offset()->is_percentage()) {
            auto flipped_percentage = 100 - offset()->as_percentage().percentage().value();
            return create({}, PercentageStyleValue::create(Percentage(flipped_percentage)));
        }

        Vector<NonnullRefPtr<CalculationNode const>> sum_parts;
        sum_parts.append(NumericCalculationNode::create(Percentage(100), context));
        if (offset()->is_length()) {
            sum_parts.append(NegateCalculationNode::create(NumericCalculationNode::create(offset()->as_length().length(), context)));
        } else {
            // FIXME: Flip calculated offsets (convert CalculatedStyleValue to CalculationNode, then negate and append)
            return *this;
        }
        auto flipped_absolute = CalculatedStyleValue::create(SumCalculationNode::create(move(sum_parts)), NumericType(NumericType::BaseType::Length, 1), context);
        return create({}, flipped_absolute);
    }

    return *this;
}

}
