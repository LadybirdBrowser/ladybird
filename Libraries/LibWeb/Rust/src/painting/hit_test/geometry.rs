/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Geometry that belongs to a row other than the item's own: its block container and the line
//! box its caret sits on. Items carry only the ids of those rows, and a query resolves the
//! rects against the committed layout, so a copied item never goes stale when a containing
//! block or line root changes without the item's own row changing.

use super::{HitTestItem, HitTestItemKind};
use crate::css::css_pixels::CssPixelRect;
use crate::layout::node_data::NodeSlotId;
use crate::painting::paintable_rows::PaintableRowsRef;
use crate::painting::{node_painting, paintable_geometry, text_fragment};

pub(crate) fn populated_block_container(rows: &PaintableRowsRef<'_>, block_container: NodeSlotId) -> NodeSlotId {
    if block_container.is_invalid() || !rows.paintable_row_is_populated(block_container) {
        return NodeSlotId::INVALID;
    }
    block_container
}

pub(crate) fn block_container_margin_rect(
    rows: &PaintableRowsRef<'_>,
    block_container: NodeSlotId,
) -> Option<CssPixelRect> {
    if populated_block_container(rows, block_container).is_invalid() {
        return None;
    }
    let absolute = paintable_geometry::absolute_rect(rows, block_container);
    let margin = paintable_geometry::committed_margin(rows, block_container);
    let border = paintable_geometry::committed_border(rows, block_container);
    let padding = paintable_geometry::committed_padding(rows, block_container);
    let content_size = paintable_geometry::committed_content_size(rows, block_container);
    let top = margin.top + border.top + padding.top;
    let right = margin.right + border.right + padding.right;
    let bottom = margin.bottom + border.bottom + padding.bottom;
    let left = margin.left + border.left + padding.left;
    Some(CssPixelRect::new(
        absolute.x - left,
        absolute.y - top,
        content_size.width + left + right,
        content_size.height + top + bottom,
    ))
}

fn absolute_containing_line_box_rect(rows: &PaintableRowsRef<'_>, paintable: NodeSlotId) -> Option<CssPixelRect> {
    let containing_line_box_index = paintable_geometry::committed_containing_line_box_index(rows, paintable)?;
    let block = rows.paintable_data(paintable).containing_block;
    if block.is_invalid() || !rows.paintable_row_is_populated(block) || !node_painting::has_lines(rows, block) {
        return None;
    }
    let side_data = rows.paintable_side_data(block);
    let line = side_data.lines().get(containing_line_box_index)?;
    Some(CssPixelRect::from(line.rect).translated_by(paintable_geometry::absolute_position(rows, block)))
}

/// The line box the item's caret belongs to, for items whose line root is another row.
pub(crate) fn containing_line_box_rect(rows: &PaintableRowsRef<'_>, item: &HitTestItem) -> Option<CssPixelRect> {
    match item.kind {
        HitTestItemKind::TextFragment => super::resolve::with_item_fragment(rows, item, |fragment| {
            text_fragment::absolute_line_box_rect(rows, item.paintable, fragment)
        }),
        HitTestItemKind::EmptyLine => Some(item.caret_rect),
        HitTestItemKind::Box => absolute_containing_line_box_rect(rows, item.paintable),
        HitTestItemKind::SvgPath | HitTestItemKind::EmptyEditable | HitTestItemKind::ChromeWidget => None,
    }
}
