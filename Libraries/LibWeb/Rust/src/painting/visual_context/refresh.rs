/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::scroll_state::{NO_SCROLL_STATE_SLOT, ScrollState, ScrollStateSlot};
use super::{SpatialData, StickyData, VisualContextTree};
use crate::css::computed_value_types::ComputedLengthPercentageOrAuto;
use crate::css::css_pixels::{CssPixelPoint, CssPixelRect, CssPixelSize, CssPixels};
use crate::layout::node_data::NodeSlotId;
use crate::painting::chrome_geometry;
use crate::painting::host::{FfiVisualContextHostCallbacks, FfiVisualContextTreeInputs};
use crate::painting::paintable_geometry;
use crate::painting::paintable_rows::PaintableRowsRead;
use crate::painting::style_queries;
use libgfx_rust::{FloatPoint, FloatRect, FloatSize};
use std::rc::Rc;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct ResolvedStickyInsets {
    pub top: Option<CssPixels>,
    pub right: Option<CssPixels>,
    pub bottom: Option<CssPixels>,
    pub left: Option<CssPixels>,
}

fn nearest_wheel_scrollable_ancestor_along_containing_blocks(
    layout_arena: &impl PaintableRowsRead,
    slot: NodeSlotId,
    tree_inputs: &FfiVisualContextTreeInputs,
) -> Option<NodeSlotId> {
    let mut block = layout_arena.paintable_data(slot).containing_block;
    while !block.is_invalid() {
        if !layout_arena.paintable_row_is_populated(block) {
            return None;
        }
        let axes = chrome_geometry::wheel_scrollable_axes(
            layout_arena,
            block,
            tree_inputs.viewport_wheel_overflow_x,
            tree_inputs.viewport_wheel_overflow_y,
        );
        if axes.horizontal || axes.vertical {
            return Some(block);
        }
        if style_queries::is_fixed_position(layout_arena, block) {
            return None;
        }
        block = layout_arena.paintable_data(block).containing_block;
    }
    None
}

// https://drafts.csswg.org/css-position/#insets
pub(crate) fn resolve_sticky_insets_in_css_pixels(
    layout_arena: &impl PaintableRowsRead,
    slot: NodeSlotId,
    tree_inputs: &FfiVisualContextTreeInputs,
) -> ResolvedStickyInsets {
    let Some(style) = layout_arena.node_style_if_live(slot) else {
        return ResolvedStickyInsets::default();
    };
    let scrollport_size = nearest_wheel_scrollable_ancestor_along_containing_blocks(layout_arena, slot, tree_inputs)
        .map_or(CssPixelSize::default(), |scroller| {
            paintable_geometry::absolute_rect(layout_arena, scroller).size()
        });
    let resolve_side = |side: &ComputedLengthPercentageOrAuto, scrollport_extent: CssPixels| {
        (!side.is_auto()).then(|| side.to_px(scrollport_extent))
    };
    let inset = &style.surround().inset;
    ResolvedStickyInsets {
        top: resolve_side(&inset.top, scrollport_size.height),
        right: resolve_side(&inset.right, scrollport_size.width),
        bottom: resolve_side(&inset.bottom, scrollport_size.height),
        left: resolve_side(&inset.left, scrollport_size.width),
    }
}

// The geometry stays zero while either row has been replaced by a subtree relayout: the pending
// tree rebuild recreates the node before anything reads it.
pub(crate) fn compute_sticky_data(
    layout_arena: &impl PaintableRowsRead,
    scroll_state: &ScrollState,
    sticky_slot: ScrollStateSlot,
    tree_inputs: &FfiVisualContextTreeInputs,
) -> StickyData {
    let device_pixels_per_css_pixel = tree_inputs.device_pixels_per_css_pixel;
    let sticky_state = scroll_state.state_at_slot(sticky_slot);
    let scroller_slot = scroll_state.nearest_scrolling_ancestor_slot(sticky_slot);
    let parent_sticky = (sticky_state.parent_slot != NO_SCROLL_STATE_SLOT
        && scroll_state.state_at_slot(sticky_state.parent_slot).is_sticky)
        .then(|| scroll_state.node_index_for_slot(sticky_state.parent_slot));
    let paintable: NodeSlotId = sticky_state.paintable;
    let mut data = StickyData::unconstrained(
        scroll_state.node_index_for_slot(scroller_slot),
        parent_sticky,
        sticky_slot,
        paintable,
        scroll_state.node_index_for_slot(sticky_state.parent_slot),
    );
    if scroller_slot == NO_SCROLL_STATE_SLOT {
        return data;
    }
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
    let device_inset = |inset: Option<CssPixels>| inset.map(|inset| inset.to_float() * scale);
    let insets = resolve_sticky_insets_in_css_pixels(layout_arena, paintable, tree_inputs);
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
    data.inset_top = device_inset(insets.top);
    data.inset_right = device_inset(insets.right);
    data.inset_bottom = device_inset(insets.bottom);
    data.inset_left = device_inset(insets.left);
    data
}

pub(crate) fn refresh_sticky_constraints(
    layout_arena: &impl PaintableRowsRead,
    scroll_state: &ScrollState,
    tree: &mut Rc<VisualContextTree>,
    tree_inputs: &FfiVisualContextTreeInputs,
) -> bool {
    let mut refreshed_sticky_payloads = Vec::new();
    for slot in 0..scroll_state.slot_count() {
        let state = scroll_state.state_at_slot(slot);
        if !state.is_sticky {
            continue;
        }
        let refreshed = compute_sticky_data(layout_arena, scroll_state, slot, tree_inputs);
        let unchanged = matches!(
            &tree.spatial_nodes[state.node_index.0 as usize].data,
            SpatialData::Sticky(current) if *current == refreshed
        );
        if !unchanged {
            refreshed_sticky_payloads.push((state.node_index, refreshed));
        }
    }
    if refreshed_sticky_payloads.is_empty() {
        return false;
    }
    let tree = Rc::make_mut(tree);
    for (node_index, refreshed) in refreshed_sticky_payloads {
        tree.spatial_nodes[node_index.0 as usize].data = SpatialData::Sticky(refreshed);
    }
    true
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
