/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::node_data::{NodeFlag, NodeKind, NodeSlotId};
use crate::layout::node_facts;
use crate::painting::paintable_rows::PaintableRowsRead;
use crate::painting::style_queries;

pub(crate) fn can_compute_client_rects_without_visual_context_update(
    arena: &impl PaintableRowsRead,
    layout_node: NodeSlotId,
    viewport_scroll_offset_is_zero: bool,
) -> bool {
    let mut node = layout_node;
    while let Some(data) = arena.node_data_if_live(node) {
        let kind = data.kind.get();
        if node_facts::kind_is_svg_box(kind) || kind == NodeKind::SVGSVGBox || kind == NodeKind::SVGForeignObjectBox {
            return false;
        }

        if !node_facts::has_flag(data, NodeFlag::HasStyle) {
            node = data.parent.get();
            continue;
        }
        if let Some(style) = arena.node_style_if_live(node)
            && (style_queries::has_css_transform(arena, node, style)
                || style.transform().has_perspective
                || style_queries::is_sticky_position(style))
        {
            return false;
        }
        let compensates_for_scroll =
            NodeFlag::CompensatesForHorizontalScroll as u32 | NodeFlag::CompensatesForVerticalScroll as u32;
        if data.flags.get() & compensates_for_scroll != 0 {
            return false;
        }
        // A scroll container's contents move, but its own border box does not.
        if node != layout_node {
            let scroll_offset_is_zero = if kind == NodeKind::Viewport {
                viewport_scroll_offset_is_zero
            } else {
                !node_facts::has_flag(data, NodeFlag::HasScrollOffset)
            };
            if !scroll_offset_is_zero && arena.paintable_row_is_populated(node) {
                return false;
            }
        }
        node = data.parent.get();
    }
    true
}
