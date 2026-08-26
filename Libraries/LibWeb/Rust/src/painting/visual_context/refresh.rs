/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::scroll_state::{NO_SCROLL_STATE_SLOT, ScrollState, ScrollStateSlot};
use super::{SpatialData, StickyData, VisualContextTree};
use crate::css::css_pixels::{CssPixelPoint, CssPixelRect, CssPixelSize, CssPixels};
use crate::layout::node_data::NodeSlotId;
use crate::painting::host::FfiVisualContextHostCallbacks;
use crate::painting::paintable_geometry;
use crate::painting::paintable_rows::PaintableRowsRead;
use libgfx_rust::{FloatPoint, FloatRect, FloatSize};

// The geometry stays zero while either row has been replaced by a subtree relayout: the pending
// tree rebuild recreates the node before anything reads it.
pub(crate) fn compute_sticky_data(
    layout_arena: &impl PaintableRowsRead,
    scroll_state: &ScrollState,
    sticky_slot: ScrollStateSlot,
    device_pixels_per_css_pixel: f64,
) -> StickyData {
    let sticky_state = scroll_state.state_at_slot(sticky_slot);
    let scroller_slot = scroll_state.nearest_scrolling_ancestor_slot(sticky_slot);
    let parent_sticky = (sticky_state.parent_slot != NO_SCROLL_STATE_SLOT
        && scroll_state.state_at_slot(sticky_state.parent_slot).is_sticky)
        .then(|| scroll_state.node_index_for_slot(sticky_state.parent_slot));
    let mut data = StickyData::unconstrained(
        scroll_state.node_index_for_slot(scroller_slot),
        parent_sticky,
        sticky_slot,
    );
    if scroller_slot == NO_SCROLL_STATE_SLOT {
        return data;
    }
    let paintable: NodeSlotId = sticky_state.paintable;
    let scroller = scroll_state.state_at_slot(scroller_slot).paintable;
    if !layout_arena.paintable_row_is_populated(paintable) || !layout_arena.paintable_row_is_populated(scroller) {
        return data;
    }

    let sticky_border_box_rect = paintable_geometry::absolute_border_box_rect(layout_arena, paintable);
    let containing_block = layout_arena.paintable_data(paintable).containing_block;
    let scroller_rect = paintable_geometry::absolute_rect(layout_arena, scroller);
    let (containing_block_region, needs_parent_offset_adjustment) = if containing_block == scroller {
        let size = paintable_geometry::scrollable_overflow_rect(layout_arena, containing_block)
            .expect("scroll ancestor has scrollable overflow")
            .size();
        (
            CssPixelRect::from_location_and_size(CssPixelPoint::default(), size),
            false,
        )
    } else {
        let region = paintable_geometry::absolute_border_box_rect(layout_arena, containing_block)
            .translated(-scroller_rect.x, -scroller_rect.y);
        (region, true)
    };

    let scale = device_pixels_per_css_pixel as f32;
    let device_point = |point: CssPixelPoint| FloatPoint {
        x: point.x.to_float() * scale,
        y: point.y.to_float() * scale,
    };
    let device_size = |size: CssPixelSize| FloatSize {
        width: size.width.to_float() * scale,
        height: size.height.to_float() * scale,
    };
    let device_inset = |inset: CssPixels, present: bool| present.then_some(inset.to_float() * scale);
    let insets = layout_arena.paintable_data(paintable).sticky_insets;
    let region_location = device_point(containing_block_region.location());
    let region_size = device_size(containing_block_region.size());

    data.position_relative_to_scroller = device_point(CssPixelPoint::new(
        sticky_border_box_rect.x - scroller_rect.x,
        sticky_border_box_rect.y - scroller_rect.y,
    ));
    data.border_box_size = device_size(sticky_border_box_rect.size());
    data.scrollport_size = device_size(scroller_rect.size());
    data.containing_block_region = FloatRect {
        x: region_location.x,
        y: region_location.y,
        width: region_size.width,
        height: region_size.height,
    };
    data.needs_parent_offset_adjustment = needs_parent_offset_adjustment;
    data.inset_top = device_inset(insets.top, insets.has_top);
    data.inset_right = device_inset(insets.right, insets.has_right);
    data.inset_bottom = device_inset(insets.bottom, insets.has_bottom);
    data.inset_left = device_inset(insets.left, insets.has_left);
    data
}

pub(crate) fn refresh_sticky_constraints(
    layout_arena: &impl PaintableRowsRead,
    scroll_state: &ScrollState,
    tree: &mut VisualContextTree,
    device_pixels_per_css_pixel: f64,
) {
    for slot in 0..scroll_state.slot_count() {
        let state = scroll_state.state_at_slot(slot);
        if !state.is_sticky {
            continue;
        }
        tree.spatial_nodes[state.node_index.0 as usize].data = SpatialData::Sticky(compute_sticky_data(
            layout_arena,
            scroll_state,
            slot,
            device_pixels_per_css_pixel,
        ));
    }
}

pub(crate) fn refresh_scroll_state(
    layout_arena: &impl PaintableRowsRead,
    callbacks: &FfiVisualContextHostCallbacks,
    scroll_state: &mut ScrollState,
) {
    for slot in 0..scroll_state.slot_count() {
        let state = scroll_state.state_at_slot(slot);
        if state.is_sticky {
            continue;
        }
        let paintable = state.paintable;
        if layout_arena.paintable_row_is_populated(paintable) {
            let offset: CssPixelPoint = callbacks.scroll_offset(layout_arena.shell_if_live(paintable)).into();
            scroll_state.state_at_slot_mut(slot).own_offset = CssPixelPoint::new(-offset.x, -offset.y);
        }
    }
}
