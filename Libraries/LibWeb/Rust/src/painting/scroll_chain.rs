/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_enums::writing_mode;
use crate::css::css_pixels::{CssPixelPoint, CssPixelRect, CssPixels};
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::painting::paintable_geometry::{absolute_padding_box_rect, has_scrollable_overflow};
use crate::painting::paintable_rows::PaintableRowsRead;
use crate::painting::{caret, chrome_geometry, scroll_snap};

fn accepted_wheel_delta(arena: &impl PaintableRowsRead, node: NodeSlotId, delta: CssPixelPoint) -> CssPixelPoint {
    let axes = chrome_geometry::wheel_scrollable_axes(arena, node);
    let zero = CssPixels::from_raw(0);
    CssPixelPoint::new(
        if axes.horizontal { delta.x } else { zero },
        if axes.vertical { delta.y } else { zero },
    )
}

fn clamp_scroll_offset(arena: &impl PaintableRowsRead, node: NodeSlotId, offset: CssPixelPoint) -> CssPixelPoint {
    let Some((minimum_offset, maximum_offset)) = chrome_geometry::scroll_offset_bounds(arena, node) else {
        return offset;
    };
    CssPixelPoint::new(
        offset.x.clamp(minimum_offset.x, maximum_offset.x),
        offset.y.clamp(minimum_offset.y, maximum_offset.y),
    )
}

fn scrolling_box_moved_by(arena: &impl PaintableRowsRead, node: NodeSlotId, delta: CssPixelPoint) -> bool {
    let current_offset = arena.row_scroll_offset(node);
    clamp_scroll_offset(arena, node, current_offset.translated(delta.x, delta.y)) != current_offset
}

pub(crate) fn scrolling_box_for_scroll_step(
    arena: &impl PaintableRowsRead,
    target: NodeSlotId,
    viewport: NodeSlotId,
    delta: CssPixelPoint,
) -> NodeSlotId {
    let scroll_step_moves = |node: NodeSlotId| {
        let accepted_delta = accepted_wheel_delta(arena, node, delta);
        accepted_delta != CssPixelPoint::default() && scrolling_box_moved_by(arena, node, accepted_delta)
    };

    let mut node = target;
    while let Some(data) = arena.node_data_if_live(node) {
        if data.kind.get() == NodeKind::Viewport {
            break;
        }
        if scroll_step_moves(node) {
            return node;
        }
        node = arena.containing_block_by_walking_ancestors(node);
    }

    if arena.slot_is_live(viewport) && scroll_step_moves(viewport) {
        return viewport;
    }
    NodeSlotId::INVALID
}

pub(crate) fn for_each_wheel_scrollable_box_in_containing_block_chain(
    arena: &impl PaintableRowsRead,
    start: NodeSlotId,
    wheel_delta_x: f64,
    wheel_delta_y: f64,
    mut push_scrollable_box: impl FnMut(NodeSlotId, f64, f64),
) {
    let mut node = start;
    while let Some(data) = arena.node_data_if_live(node) {
        if data.kind.get() != NodeKind::Viewport {
            let axes = chrome_geometry::wheel_scrollable_axes(arena, node);
            let accepted_delta_x = if axes.horizontal { wheel_delta_x } else { 0.0 };
            let accepted_delta_y = if axes.vertical { wheel_delta_y } else { 0.0 };
            if accepted_delta_x != 0.0 || accepted_delta_y != 0.0 {
                push_scrollable_box(node, accepted_delta_x, accepted_delta_y);
                let accepted_delta = CssPixelPoint::new(
                    CssPixels::nearest_value_for(accepted_delta_x),
                    CssPixels::nearest_value_for(accepted_delta_y),
                );
                if scrolling_box_moved_by(arena, node, accepted_delta) {
                    return;
                }
            }
        }
        node = arena.containing_block_by_walking_ancestors(node);
    }
}

pub(crate) fn first_wheel_scrollable_box_in_containing_block_chain(
    arena: &impl PaintableRowsRead,
    start: NodeSlotId,
) -> NodeSlotId {
    let mut node = start;
    while let Some(data) = arena.node_data_if_live(node) {
        let backed_by_element_or_viewport = data.kind.get() == NodeKind::Viewport || arena.node_is_element_backed(node);
        if backed_by_element_or_viewport && arena.paintable_row_is_populated(node) {
            let axes = chrome_geometry::wheel_scrollable_axes(arena, node);
            if axes.horizontal || axes.vertical {
                return node;
            }
        }
        node = arena.containing_block_by_walking_ancestors(node);
    }
    NodeSlotId::INVALID
}

/// The box a scroll brings a text position's caret into view in, and the offset it scrolls that box to: the nearest box
/// with scrollable overflow in the containing block chain of the caret's box. Where `scroll_block_axis` is false, the
/// box keeps its block axis offset.
pub(crate) fn scroll_target_for_text_position(
    arena: &impl PaintableRowsRead,
    text: NodeSlotId,
    offset: usize,
    affinity_is_downstream: bool,
    scroll_block_axis: bool,
) -> Option<(NodeSlotId, CssPixelPoint)> {
    let fragments = arena.text_fragments(text);
    let caret = caret::caret_rect_for_position(arena, fragments.as_slice(), offset, affinity_is_downstream)?;
    let style = arena.node_style_if_live(caret.style_source)?;
    let horizontal = style.writing_mode() == writing_mode::HORIZONTAL_TB;
    let one = CssPixels::from_integer(1);

    let mut rect = caret.rect;
    if horizontal {
        if style.inline_axis_is_reverse() {
            rect.x -= one;
        }
        rect.width = one;
    } else {
        if style.inline_axis_is_reverse() {
            rect.y -= one;
        }
        rect.height = one;
    }

    let mut container = caret.owner;
    while !has_scrollable_overflow(arena, container) {
        container = arena
            .node_containing_block_if_live(container)
            .filter(|&containing_block| arena.paintable_row_is_populated(containing_block))?;
    }
    let snapport = scroll_snap::scroll_snapport_rect(arena, container, absolute_padding_box_rect(arena, container));
    let current_offset = arena.row_scroll_offset(container);
    if !scroll_block_axis {
        if horizontal {
            rect.y = snapport.y + current_offset.y;
            rect.height = snapport.height;
        } else {
            rect.x = snapport.x + current_offset.x;
            rect.width = snapport.width;
        }
    }
    Some((container, offset_showing_rect(rect, snapport, current_offset)))
}

// The nearest offset to `current_offset` at which `rect`, in the same space as `snapport`, lies inside the snapport.
fn offset_showing_rect(rect: CssPixelRect, snapport: CssPixelRect, current_offset: CssPixelPoint) -> CssPixelPoint {
    let axis = |start: CssPixels, end: CssPixels, size: CssPixels, current: CssPixels| {
        if end > current + size {
            end - size
        } else if start < current {
            start
        } else {
            current
        }
    };
    let rect = rect.translated(-snapport.x, -snapport.y);
    CssPixelPoint::new(
        axis(rect.left(), rect.right(), snapport.width, current_offset.x),
        axis(rect.top(), rect.bottom(), snapport.height, current_offset.y),
    )
}
