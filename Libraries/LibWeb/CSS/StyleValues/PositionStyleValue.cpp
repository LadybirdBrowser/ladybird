/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PositionStyleValue.h"

namespace Web::CSS {

bool PositionStyleValue::is_center(SerializationMode mode) const
{
    return edge_x()->is_center(mode) && edge_y()->is_center(mode);
}

CSSPixelPoint PositionStyleValue::resolved(Layout::Node const& node, CSSPixelRect const& rect) const
{
    // Note: A preset + a none default x/y_relative_to is impossible in the syntax (and makes little sense)
    CSSPixels x = LengthPercentage::from_style_value(m_properties.edge_x->offset()).to_px(node, rect.width());
    CSSPixels y = LengthPercentage::from_style_value(m_properties.edge_y->offset()).to_px(node, rect.height());
    return CSSPixelPoint { rect.x() + x, rect.y() + y };
}

ValueComparingNonnullRefPtr<PositionStyleValue const> PositionStyleValue::with_resolved_keywords() const
{
    return PositionStyleValue::create(
        edge_x()->with_resolved_keywords(),
        edge_y()->with_resolved_keywords());
}

ValueComparingNonnullRefPtr<StyleValue const> PositionStyleValue::absolutized(ComputationContext const& computation_context) const
{
    return PositionStyleValue::create(
        edge_x()->absolutized(computation_context)->as_edge(),
        edge_y()->absolutized(computation_context)->as_edge());
}

String PositionStyleValue::to_string(SerializationMode mode) const
{
    return MUST(String::formatted("{} {}", m_properties.edge_x->to_string(mode), m_properties.edge_y->to_string(mode)));
}

}
