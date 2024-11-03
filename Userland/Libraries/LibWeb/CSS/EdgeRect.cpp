/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "EdgeRect.h"

namespace Web::CSS {

// https://www.w3.org/TR/CSS2/visufx.html#value-def-shape
CSSPixelRect EdgeRect::resolved(Layout::Node const& layout_node, CSSPixelRect border_box) const
{
    // In CSS 2.1, the only valid <shape> value is: rect(<top>, <right>, <bottom>, <left>) where
    // <top> and <bottom> specify offsets from the top border edge of the box, and <right>, and
    // <left> specify offsets from the left border edge of the box.

    // The value 'auto' means that a given edge of the clipping region will be the same as the edge
    // of the element's generated border box (i.e., 'auto' means the same as '0' for <top> and
    // <left>, the same as the used value of the height plus the sum of vertical padding and border
    // widths for <bottom>, and the same as the used value of the width plus the sum of the
    // horizontal padding and border widths for <right>, such that four 'auto' values result in the
    // clipping region being the same as the element's border box).
    auto left = border_box.left() + (left_edge.is_auto() ? 0 : left_edge.to_px(layout_node));
    auto top = border_box.top() + (top_edge.is_auto() ? 0 : top_edge.to_px(layout_node));
    auto right = border_box.left() + (right_edge.is_auto() ? border_box.width() : right_edge.to_px(layout_node));
    auto bottom = border_box.top() + (bottom_edge.is_auto() ? border_box.height() : bottom_edge.to_px(layout_node));
    return CSSPixelRect {
        left,
        top,
        right - left,
        bottom - top
    };
}

}
