/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BorderRadiusRectStyleValue.h"
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>

namespace Web::CSS {

void BorderRadiusRectStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    auto horizontal_radii_serialized = serialize_a_positional_value_list(
        { m_top_left->as_border_radius().horizontal_radius(), m_top_right->as_border_radius().horizontal_radius(), m_bottom_right->as_border_radius().horizontal_radius(), m_bottom_left->as_border_radius().horizontal_radius() },
        mode);

    auto vertical_radii_serialized = serialize_a_positional_value_list(
        { m_top_left->as_border_radius().vertical_radius(), m_top_right->as_border_radius().vertical_radius(), m_bottom_right->as_border_radius().vertical_radius(), m_bottom_left->as_border_radius().vertical_radius() },
        mode);

    if (horizontal_radii_serialized == vertical_radii_serialized) {
        builder.append(horizontal_radii_serialized);
        return;
    }

    builder.appendff("{} / {}", horizontal_radii_serialized, vertical_radii_serialized);
}

ValueComparingNonnullRefPtr<StyleValue const> BorderRadiusRectStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto top_left_absolutized = m_top_left->absolutized(computation_context);
    auto top_right_absolutized = m_top_right->absolutized(computation_context);
    auto bottom_right_absolutized = m_bottom_right->absolutized(computation_context);
    auto bottom_left_absolutized = m_bottom_left->absolutized(computation_context);

    return BorderRadiusRectStyleValue::create(top_left_absolutized, top_right_absolutized, bottom_right_absolutized, bottom_left_absolutized);
}

}
