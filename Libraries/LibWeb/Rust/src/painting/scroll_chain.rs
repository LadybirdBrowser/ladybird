/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::{CssPixelPoint, CssPixels};
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::painting::chrome_geometry;
use crate::painting::paintable_rows::PaintableRowsRead;

#[derive(Clone, Copy)]
pub(crate) struct ViewportWheelOverflow {
    pub x: u8,
    pub y: u8,
}

fn wheel_scrollable_axes(
    arena: &impl PaintableRowsRead,
    node: NodeSlotId,
    viewport_wheel_overflow: ViewportWheelOverflow,
) -> chrome_geometry::PhysicalAxes {
    chrome_geometry::wheel_scrollable_axes(arena, node, viewport_wheel_overflow.x, viewport_wheel_overflow.y)
}

fn accepted_wheel_delta(
    arena: &impl PaintableRowsRead,
    node: NodeSlotId,
    viewport_wheel_overflow: ViewportWheelOverflow,
    delta: CssPixelPoint,
) -> CssPixelPoint {
    let axes = wheel_scrollable_axes(arena, node, viewport_wheel_overflow);
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

fn scrolling_box_moved_by(
    arena: &impl PaintableRowsRead,
    node: NodeSlotId,
    delta: CssPixelPoint,
    scroll_offset_of_layout_node: &dyn Fn(NodeSlotId) -> CssPixelPoint,
) -> bool {
    let current_offset = scroll_offset_of_layout_node(node);
    clamp_scroll_offset(arena, node, current_offset.translated(delta.x, delta.y)) != current_offset
}

pub(crate) fn scrolling_box_for_scroll_step(
    arena: &impl PaintableRowsRead,
    target: NodeSlotId,
    viewport: NodeSlotId,
    delta: CssPixelPoint,
    viewport_wheel_overflow: ViewportWheelOverflow,
    scroll_offset_of_layout_node: &dyn Fn(NodeSlotId) -> CssPixelPoint,
) -> NodeSlotId {
    let scroll_step_moves = |node: NodeSlotId| {
        let accepted_delta = accepted_wheel_delta(arena, node, viewport_wheel_overflow, delta);
        accepted_delta != CssPixelPoint::default()
            && scrolling_box_moved_by(arena, node, accepted_delta, scroll_offset_of_layout_node)
    };

    let mut node = target;
    while let Some(data) = arena.node_data_if_live(node) {
        if data.kind.get() == NodeKind::Viewport {
            break;
        }
        if scroll_step_moves(node) {
            return node;
        }
        node = data.containing_block.get();
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
    viewport_wheel_overflow: ViewportWheelOverflow,
    scroll_offset_of_layout_node: &dyn Fn(NodeSlotId) -> CssPixelPoint,
    mut push_scrollable_box: impl FnMut(NodeSlotId, f64, f64),
) {
    let mut node = start;
    while let Some(data) = arena.node_data_if_live(node) {
        if data.kind.get() != NodeKind::Viewport {
            let axes = wheel_scrollable_axes(arena, node, viewport_wheel_overflow);
            let accepted_delta_x = if axes.horizontal { wheel_delta_x } else { 0.0 };
            let accepted_delta_y = if axes.vertical { wheel_delta_y } else { 0.0 };
            if accepted_delta_x != 0.0 || accepted_delta_y != 0.0 {
                push_scrollable_box(node, accepted_delta_x, accepted_delta_y);
                let accepted_delta = CssPixelPoint::new(
                    CssPixels::nearest_value_for(accepted_delta_x),
                    CssPixels::nearest_value_for(accepted_delta_y),
                );
                if scrolling_box_moved_by(arena, node, accepted_delta, scroll_offset_of_layout_node) {
                    return;
                }
            }
        }
        node = data.containing_block.get();
    }
}

pub(crate) fn first_wheel_scrollable_box_in_containing_block_chain(
    arena: &impl PaintableRowsRead,
    start: NodeSlotId,
    viewport_wheel_overflow: ViewportWheelOverflow,
) -> NodeSlotId {
    let mut node = start;
    while let Some(data) = arena.node_data_if_live(node) {
        let backed_by_element_or_viewport =
            data.kind.get() == NodeKind::Viewport || arena.node_dom_node_is_element(node);
        if backed_by_element_or_viewport && arena.paintable_row_is_populated(node) {
            let axes = wheel_scrollable_axes(arena, node, viewport_wheel_overflow);
            if axes.horizontal || axes.vertical {
                return node;
            }
        }
        node = data.containing_block.get();
    }
    NodeSlotId::INVALID
}
