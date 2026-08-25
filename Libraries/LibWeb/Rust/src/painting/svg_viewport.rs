/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::node_data::NodeSlotId;
use crate::painting::paintable_data::PaintableKind;
use crate::painting::paintable_geometry;
use crate::painting::paintable_rows::PaintableRowsRead;
use libgfx_rust::FloatRect;

pub(crate) fn nearest_svg_viewport_of(arena: &impl PaintableRowsRead, slot: NodeSlotId) -> Option<NodeSlotId> {
    let mut ancestor = arena.node_parent_if_live(slot);
    while let Some(node) = ancestor {
        if paintable_geometry::committed_svg_viewport_transform(arena, node).is_some() {
            return Some(node);
        }
        ancestor = arena.node_parent_if_live(node);
    }
    None
}

pub(crate) fn svg_viewport_user_rect(arena: &impl PaintableRowsRead, viewport: NodeSlotId) -> FloatRect {
    if let Some(view_box) = paintable_geometry::committed_svg_view_box(arena, viewport) {
        return FloatRect::new(
            view_box.min_x as f32,
            view_box.min_y as f32,
            view_box.width as f32,
            view_box.height as f32,
        );
    }

    let size = if arena.paintable_data(viewport).kind == PaintableKind::SVGSVGPaintable {
        let size = paintable_geometry::committed_svg_viewport_size(arena, viewport);
        (size.width.to_float(), size.height.to_float())
    } else {
        let rect = paintable_geometry::absolute_rect(arena, viewport);
        (rect.width.to_float(), rect.height.to_float())
    };
    FloatRect::new(0.0, 0.0, size.0, size.1)
}

pub(crate) fn nearest_svg_viewport_user_rect(arena: &impl PaintableRowsRead, slot: NodeSlotId) -> Option<FloatRect> {
    nearest_svg_viewport_of(arena, slot).map(|viewport| svg_viewport_user_rect(arena, viewport))
}
