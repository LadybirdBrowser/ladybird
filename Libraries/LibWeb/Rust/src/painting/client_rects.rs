/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::layout::node_data::{NodeFlag, NodeKind, NodeSlotId};
use crate::layout::node_facts;
use crate::painting::node_painting;
use crate::painting::paintable_geometry;
use crate::painting::paintable_rows::{PaintableRowsRead, with_inline_pieces};
use crate::painting::rect_to_viewport_transform::{RectToViewportTransform, transform_rect_to_viewport_or_identity};
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

fn for_each_inline_piece_border_box_rect(
    arena: &impl PaintableRowsRead,
    inline_paintable: NodeSlotId,
    mut push_rect: impl FnMut(CssPixelRect),
) {
    let Some(root) = arena.inline_pieces_root(inline_paintable) else {
        return;
    };
    let root_position = paintable_geometry::absolute_position(arena, root);
    with_inline_pieces(arena, inline_paintable, |piece, _| {
        if piece.is_geometry_only_placeholder {
            return true;
        }
        push_rect(CssPixelRect::from(piece.border_box_rect).translated_by(root_position));
        true
    });
}

// https://drafts.csswg.org/cssom-view/#dom-element-getclientrects
pub(crate) fn for_each_client_rect(
    arena: &impl PaintableRowsRead,
    layout_node: NodeSlotId,
    rect_to_viewport_transform: Option<&RectToViewportTransform<'_>>,
    mut push_rect: impl FnMut(CssPixelRect),
) {
    let mut push_rect_in_viewport_space = |rect: CssPixelRect| {
        push_rect(transform_rect_to_viewport_or_identity(
            rect_to_viewport_transform,
            arena,
            layout_node,
            rect,
        ));
    };
    let row_is_populated = arena.paintable_row_is_populated(layout_node);

    // FIXME: 2. If the element has an associated SVG layout box return a DOMRectList object containing a single
    //          DOMRect object that describes the bounding box of the element as defined by the SVG specification,
    //          applying the transforms that apply to the element and its ancestors.

    // 3. Return a DOMRectList object containing DOMRect objects in content order, one for each box fragment,
    // describing its border area (including those with a height or width of zero) with the following constraints:
    // - Apply the transforms that apply to the element and its ancestors.
    // FIXME: - If the element on which the method was invoked has a computed value for the display property of table
    //          or inline-table include both the table box and the caption box, if any, but not the anonymous container box.
    // FIXME: - Replace each anonymous block box with its child box(es) and repeat this until no anonymous block boxes
    //          are left in the final list.
    if row_is_populated && node_painting::is_fragmented_inline(arena, layout_node) {
        let mut pushed_a_piece_rect = false;
        for_each_inline_piece_border_box_rect(arena, layout_node, |piece_border_box_rect| {
            pushed_a_piece_rect = true;
            push_rect_in_viewport_space(piece_border_box_rect);
        });
        // An inline element whose content is only interrupting blocks generates no line fragments, but per CSSOM
        // we still report its (zero-sized) border area instead of an empty list.
        if !pushed_a_piece_rect {
            push_rect_in_viewport_space(paintable_geometry::absolute_border_box_rect(arena, layout_node));
        }
        return;
    }

    if row_is_populated {
        push_rect_in_viewport_space(paintable_geometry::absolute_border_box_rect(arena, layout_node));
    }
}

// https://drafts.csswg.org/cssom-view/#dom-element-getboundingclientrect
pub(crate) fn bounding_client_rect(
    arena: &impl PaintableRowsRead,
    layout_node: NodeSlotId,
    rect_to_viewport_transform: Option<&RectToViewportTransform<'_>>,
) -> CssPixelRect {
    // 1. Let list be the result of invoking getClientRects() on element.
    let zero = CssPixels::from_integer(0);
    let mut first_rect = None;
    let mut union_of_rects_with_nonzero_width_and_height: Option<CssPixelRect> = None;
    for_each_client_rect(arena, layout_node, rect_to_viewport_transform, |rect| {
        first_rect.get_or_insert(rect);
        if rect.width == zero || rect.height == zero {
            return;
        }
        match &mut union_of_rects_with_nonzero_width_and_height {
            None => union_of_rects_with_nonzero_width_and_height = Some(rect),
            Some(union) => union.unite(rect),
        }
    });

    // 2. If the list is empty return a DOMRect object whose x, y, width and height members are zero.
    let Some(first_rect) = first_rect else {
        return CssPixelRect::default();
    };

    // 3. If all rectangles in list have zero width or height, return the first rectangle in list.

    // 4. Otherwise, return a DOMRect object describing the smallest rectangle that includes all of the rectangles in
    //    list of which the height or width is not zero.
    union_of_rects_with_nonzero_width_and_height.unwrap_or(first_rect)
}
