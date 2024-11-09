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

bool PositionStyleValue::is_center() const
{
    return (edge_x()->edge() == PositionEdge::Left
               && edge_x()->offset().is_percentage() && edge_x()->offset().percentage() == Percentage { 50 })
        && (edge_y()->edge() == PositionEdge::Top
            && edge_y()->offset().is_percentage() && edge_y()->offset().percentage() == Percentage { 50 });
}

CSSPixelPoint PositionStyleValue::resolved(Layout::Node const& node, CSSPixelRect const& rect) const
{
    // Note: A preset + a none default x/y_relative_to is impossible in the syntax (and makes little sense)
    CSSPixels x = m_properties.edge_x->offset().to_px(node, rect.width());
    CSSPixels y = m_properties.edge_y->offset().to_px(node, rect.height());
    if (m_properties.edge_x->edge() == PositionEdge::Right)
        x = rect.width() - x;
    if (m_properties.edge_y->edge() == PositionEdge::Bottom)
        y = rect.height() - y;
    return CSSPixelPoint { rect.x() + x, rect.y() + y };
}

String PositionStyleValue::to_string() const
{
    return MUST(String::formatted("{} {}", m_properties.edge_x->to_string(), m_properties.edge_y->to_string()));
}

}
