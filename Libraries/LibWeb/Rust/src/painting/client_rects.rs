/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::layout::inline_content::InlineBoxPieceRecord;
use crate::layout::node_data::{NodeFlag, NodeKind, NodeSlotId};
use crate::layout::node_facts::{self, NodeShape};
use crate::painting::node_painting;
use crate::painting::paint_read::{PaintRead, PaintRow};
use crate::painting::paintable_geometry;
use crate::painting::paintable_rows::with_inline_pieces;
use crate::painting::rect_to_viewport_transform::{RectToViewportTransform, transform_rect_to_viewport_or_identity};
use crate::painting::style_queries;

pub(crate) fn can_compute_client_rects_without_visual_context_update(
    arena: &impl PaintRead,
    layout_node: NodeSlotId,
    viewport_scroll_offset_is_zero: bool,
) -> bool {
    let mut node = layout_node;
    while let Some(row) = arena.node(node) {
        let (kind, flags, parent) = (row.kind(), row.flags(), row.parent());
        if node_facts::kind_is_svg_box(kind) || kind == NodeKind::SVGSVGBox || kind == NodeKind::SVGForeignObjectBox {
            return false;
        }

        if flags & NodeFlag::HasStyle as u32 == 0 {
            node = parent;
            continue;
        }
        if let Some(style) = row.style()
            && (style_queries::has_css_transform(arena, node, style)
                || style.transform().has_perspective
                || style_queries::is_sticky_position(style))
        {
            return false;
        }
        let compensates_for_scroll =
            NodeFlag::CompensatesForHorizontalScroll as u32 | NodeFlag::CompensatesForVerticalScroll as u32;
        if flags & compensates_for_scroll != 0 {
            return false;
        }
        // A scroll container's contents move, but its own border box does not.
        if node != layout_node {
            let scroll_offset_is_zero = if kind == NodeKind::Viewport {
                viewport_scroll_offset_is_zero
            } else {
                flags & NodeFlag::HasScrollOffset as u32 == 0
            };
            if !scroll_offset_is_zero && arena.paintable_row_is_populated(node) {
                return false;
            }
        }
        node = parent;
    }
    true
}

/// The border boxes of the fragments of an inline box, in content order: its pieces on the lines, and the in-flow
/// block-level boxes inside it, which break it around themselves.
fn for_each_inline_fragment_border_box_rect(
    arena: &impl PaintRead,
    inline_paintable: NodeSlotId,
    mut push_rect: impl FnMut(CssPixelRect),
) {
    let Some(root) = arena.inline_pieces_root(inline_paintable) else {
        return;
    };
    let root_positions = paintable_geometry::LineRootPositions::new(arena, root);
    let root_side = arena.committed_side_data(root);
    let mut pushed_a_rect = false;
    let mut empty_piece_after_last_block = None;
    with_inline_pieces(arena, inline_paintable, |piece| {
        if piece.is_geometry_only_placeholder {
            // A placeholder piece on a line that a block-level box takes up stands for that box, if the box is inside
            // the inline box. (Empty inline boxes get placeholder pieces too.)
            if let Some(block) = interrupting_block(arena, root, inline_paintable, piece) {
                // https://drafts.csswg.org/css2/#anonymous-block-level
                // When an inline box contains an in-flow block-level box, the inline box (and its inline ancestors
                // within the same line box) is broken around the block-level box (and any block-level siblings
                // that are consecutive or separated only by collapsible whitespace and/or out-of-flow elements),
                // splitting the inline box into two boxes (even if either side is empty), one on each side
                // of the block-level box(es).
                // NB: So the empty part of the inline box before the first block gets a rect when nothing of
                //     it precedes that block, and the empty part after the last block gets one when nothing
                //     of it follows that block.
                if !pushed_a_rect {
                    push_rect(empty_piece_rect(&block, false));
                }
                push_rect(block.border_box_rect);
                pushed_a_rect = true;
                empty_piece_after_last_block = Some(empty_piece_rect(&block, true));
            }
            return true;
        }
        let root_position = root_positions.of_line(root_side.lines(), piece.line_index as usize);
        push_rect(CssPixelRect::from(piece.border_box_rect).translated_by(root_position));
        pushed_a_rect = true;
        empty_piece_after_last_block = None;
        true
    });
    if let Some(rect) = empty_piece_after_last_block {
        push_rect(rect);
    }
}

struct InterruptingBlock {
    border_box_rect: CssPixelRect,
    /// The margin box the block would have without its relative offset.
    normal_flow_margin_box_rect: CssPixelRect,
    /// The content box of the block's line root, on the block's line.
    line_root_content_rect: CssPixelRect,
    writing_mode: u8,
    direction: u8,
}

/// The block-level box that takes up the line that the placeholder `piece` of `inline_paintable` is on, if that box is
/// inside `inline_paintable`.
fn interrupting_block(
    arena: &impl PaintRead,
    line_root: NodeSlotId,
    inline_paintable: NodeSlotId,
    piece: &InlineBoxPieceRecord,
) -> Option<InterruptingBlock> {
    let root_side = arena.committed_side_data(line_root);
    let line = root_side.lines().get(piece.line_index as usize)?;
    if !line.has_block_level_box {
        return None;
    }
    // NOTE: A line taken up by a block-level box holds that box's fragment and nothing else.
    let first = piece.first_fragment_index as usize;
    let fragment = root_side
        .fragments()
        .get(first..first + piece.fragment_count as usize)?
        .iter()
        .find(|fragment| arena.paintable_row_is_populated(fragment.layout_node))?;
    let mut ancestor = arena.node(fragment.layout_node)?.parent();
    while ancestor != inline_paintable {
        if ancestor == line_root {
            return None;
        }
        ancestor = arena.node(ancestor)?.parent();
    }
    let border_box_rect = paintable_geometry::absolute_border_box_rect(arena, fragment.layout_node);
    // NOTE: For an in-flow box, the insets are its relative offset, and zero unless it's relatively positioned.
    let relative_offset = paintable_geometry::committed_inset(arena, fragment.layout_node);
    let margin = paintable_geometry::committed_margin(arena, fragment.layout_node);
    let line_root_content_size = paintable_geometry::committed_content_size(arena, line_root);
    Some(InterruptingBlock {
        border_box_rect,
        normal_flow_margin_box_rect: CssPixelRect {
            x: border_box_rect.x - relative_offset.left - margin.left,
            y: border_box_rect.y - relative_offset.top - margin.top,
            width: border_box_rect.width + margin.left + margin.right,
            height: border_box_rect.height + margin.top + margin.bottom,
        },
        line_root_content_rect: CssPixelRect::from_location_and_size(
            paintable_geometry::absolute_line_root_position(arena, line_root, piece.line_index as usize),
            line_root_content_size.into(),
        ),
        writing_mode: line.writing_mode,
        direction: line.direction,
    })
}

/// The zero-size rect of an empty part of an inline box that a block-level box breaks. The parts are on the anonymous
/// lines around the block, so they're at the line's start (the inline-start edge of the block's margin box), and at the
/// block-start edge of the block's margin box for the part before the block (but not before the start of the line
/// root's content, which a margin that collapses through the line root can reach past), or its block-end edge for the
/// part after it. The parts are in normal flow, so a relative offset of the block doesn't move them.
fn empty_piece_rect(block: &InterruptingBlock, after_the_block: bool) -> CssPixelRect {
    use crate::css::css_enums::{direction, writing_mode};
    let inline_axis_is_reverse = match block.writing_mode {
        writing_mode::SIDEWAYS_LR => block.direction == direction::LTR,
        _ => block.direction == direction::RTL,
    };
    let block_axis_is_reverse = matches!(
        block.writing_mode,
        writing_mode::VERTICAL_RL | writing_mode::SIDEWAYS_RL
    );
    let at_block_axis_high_side = after_the_block != block_axis_is_reverse;
    let rect = block.normal_flow_margin_box_rect;
    let content = block.line_root_content_rect;
    // The block-axis position of the part on the block-axis low or high side, as above.
    let block_axis_position = |low: CssPixels, size: CssPixels, content_low: CssPixels, content_size: CssPixels| match (
        at_block_axis_high_side,
        after_the_block,
    ) {
        (true, true) => low + size,
        (false, true) => low,
        (true, false) => (low + size).min(content_low + content_size),
        (false, false) => low.max(content_low),
    };
    let (x, y) = if block.writing_mode == writing_mode::HORIZONTAL_TB {
        (
            if inline_axis_is_reverse {
                rect.x + rect.width
            } else {
                rect.x
            },
            block_axis_position(rect.y, rect.height, content.y, content.height),
        )
    } else {
        (
            block_axis_position(rect.x, rect.width, content.x, content.width),
            if inline_axis_is_reverse {
                rect.y + rect.height
            } else {
                rect.y
            },
        )
    };
    let zero = CssPixels::from_integer(0);
    CssPixelRect {
        x,
        y,
        width: zero,
        height: zero,
    }
}

// https://drafts.csswg.org/cssom-view/#dom-element-getclientrects
pub(crate) fn for_each_client_rect(
    arena: &impl PaintRead,
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
    // INTEROP: A line break has no box fragments of its own, but browsers report a zero-width rect where it ends its
    //          line.
    if arena.node_kind_if_live(layout_node) == Some(NodeKind::BreakNode) {
        if let Some((line_root, rect)) = line_break_rect(arena, layout_node) {
            push_rect(transform_rect_to_viewport_or_identity(
                rect_to_viewport_transform,
                arena,
                line_root,
                rect,
            ));
        }
        return;
    }

    let row_is_populated = arena.paintable_row_is_populated(layout_node);

    // FIXME: 2. If the element has an associated SVG layout box return a DOMRectList object containing a single
    //          DOMRect object that describes the bounding box of the element as defined by the SVG specification,
    //          applying the transforms that apply to the element and its ancestors.

    // 3. Return a DOMRectList object containing DOMRect objects in content order, one for each box fragment,
    // describing its border area (including those with a height or width of zero) with the following constraints:
    // - Apply the transforms that apply to the element and its ancestors.
    // FIXME: - If the element on which the method was invoked has a computed value for the display property of table
    //          or inline-table include both the table box and the caption box, if any, but not the anonymous container box.
    // - Replace each anonymous block box with its child box(es) and repeat this until no anonymous block boxes are left
    //   in the final list.
    // NB: The anonymous block boxes that wrap a block-level box inside an inline box are replaced by that block-level
    //     box, which is one of the fragments of the inline box it breaks.
    if row_is_populated && node_painting::is_fragmented_inline(arena, layout_node) {
        let mut pushed_a_piece_rect = false;
        for_each_inline_fragment_border_box_rect(arena, layout_node, |piece_border_box_rect| {
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
        for index in 0..paintable_geometry::fragment_count(arena, layout_node) {
            push_rect_in_viewport_space(paintable_geometry::absolute_fragment_border_box_rect(
                arena,
                layout_node,
                index,
            ));
        }
    }
}

/// The line root whose line the forced line break ends, and the zero-width rect of the break at the inline end of
/// that line's content, or at the inline start of the line where it is empty.
fn line_break_rect(arena: &impl PaintRead, break_node: NodeSlotId) -> Option<(NodeSlotId, CssPixelRect)> {
    let mut ancestor = arena.node(break_node)?.parent();
    while let Some(row) = arena.node(ancestor) {
        if arena.paintable_row_is_populated(ancestor) {
            let side = arena.committed_side_data(ancestor);
            if let Some(line) = side.lines().iter().find(|line| line.forced_break_node == break_node) {
                let style = arena.node_style_if_live(ancestor)?;
                let root_position =
                    paintable_geometry::LineRootPositions::new(arena, ancestor).of_fragment(line.box_fragment);
                let line_rect = CssPixelRect::from(line.rect).translated_by(root_position);
                let inline_axis_is_horizontal =
                    style.writing_mode() == crate::css::css_enums::writing_mode::HORIZONTAL_TB;
                let at_high_side = (line.fragment_count == 0) == style.inline_axis_is_reverse();
                let zero = CssPixels::default();
                let rect = match (inline_axis_is_horizontal, at_high_side) {
                    (true, false) => CssPixelRect {
                        width: zero,
                        ..line_rect
                    },
                    (true, true) => CssPixelRect {
                        x: line_rect.x + line_rect.width,
                        width: zero,
                        ..line_rect
                    },
                    (false, false) => CssPixelRect {
                        height: zero,
                        ..line_rect
                    },
                    (false, true) => CssPixelRect {
                        y: line_rect.y + line_rect.height,
                        height: zero,
                        ..line_rect
                    },
                };
                return Some((ancestor, rect));
            }
        }
        ancestor = row.parent();
    }
    None
}

// https://drafts.csswg.org/cssom-view/#dom-element-getboundingclientrect
pub(crate) fn bounding_client_rect(
    arena: &impl PaintRead,
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
