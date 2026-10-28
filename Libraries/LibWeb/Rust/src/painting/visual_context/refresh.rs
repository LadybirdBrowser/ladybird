/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::scroll_state::{NO_SCROLL_STATE_SLOT, ScrollState, ScrollStateSlot};
use super::{SpatialData, StickyData, VisualContextTree};
use crate::css::computed_value_types::ComputedLengthPercentageOrAuto;
use crate::css::css_pixels::{CssPixelPoint, CssPixelRect, CssPixelSize, CssPixels};
use crate::layout::geometry::{Frame, Side};
use crate::layout::node_data::NodeSlotId;
use crate::painting::host::FfiVisualContextTreeInputs;
use crate::painting::paintable_data::FfiPixelBox;
use crate::painting::paintable_geometry;
use crate::painting::paintable_rows::PaintableRowsRead;
use crate::painting::style_queries;
use libgfx_rust::{FloatPoint, FloatRect, FloatSize};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct ResolvedStickyInsets {
    pub top: Option<CssPixels>,
    pub right: Option<CssPixels>,
    pub bottom: Option<CssPixels>,
    pub left: Option<CssPixels>,
}

// https://drafts.csswg.org/css-position/#stickypos-insets
// The sticky view rectangle comes from the nearest scroll container, even one without scrollable overflow.
fn nearest_scroll_container_along_containing_blocks(
    layout_arena: &impl PaintableRowsRead,
    slot: NodeSlotId,
) -> Option<NodeSlotId> {
    let mut block = layout_arena.paintable_data(slot).containing_block;
    while !block.is_invalid() {
        if !layout_arena.paintable_row_is_populated(block) {
            return None;
        }
        if style_queries::is_scroll_container(layout_arena, block) {
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
fn resolve_sticky_insets_in_css_pixels(
    layout_arena: &impl PaintableRowsRead,
    slot: NodeSlotId,
    scrollport_size: CssPixelSize,
    border_box_size: CssPixelSize,
) -> ResolvedStickyInsets {
    let Some(style) = layout_arena.node_style_if_live(slot) else {
        return ResolvedStickyInsets::default();
    };
    let resolve_side = |side: &ComputedLengthPercentageOrAuto, scrollport_extent: CssPixels| {
        (!side.is_auto()).then(|| side.to_px(scrollport_extent))
    };
    let inset = &style.surround().inset;
    let mut insets = [
        resolve_side(&inset.top, scrollport_size.height),
        resolve_side(&inset.right, scrollport_size.width),
        resolve_side(&inset.bottom, scrollport_size.height),
        resolve_side(&inset.left, scrollport_size.width),
    ];
    // https://drafts.csswg.org/css-position-3/#sticky-pos
    // If this results in a sticky view rectangle size in any axis less than the size of the border box of the sticky
    // box in that axis, then the effective end-edge inset in the affected axis is reduced (possibly becoming negative)
    // to bring the sticky view rectangle’s size up to the size of the border box in that axis (where end is
    // interpreted relative to the writing mode of the containing block).
    let containing_block = layout_arena.paintable_data(slot).containing_block;
    let containing_style = layout_arena.node_style_if_live(containing_block);
    let frame = containing_style.map_or(Frame::PHYSICAL, |style| Frame::of(style.writing_mode()));
    let inline_end = if containing_style.is_some_and(|style| style.direction() == crate::css::css_enums::direction::RTL)
    {
        Side::Left
    } else {
        Side::Right
    };
    for end in [frame.physical_side(inline_end), frame.physical_side(Side::Bottom)] {
        let (first, second, extent, border_extent) = match end {
            Side::Top | Side::Bottom => (Side::Top, Side::Bottom, scrollport_size.height, border_box_size.height),
            Side::Left | Side::Right => (Side::Left, Side::Right, scrollport_size.width, border_box_size.width),
        };
        let view_extent =
            extent - insets[first as usize].unwrap_or_default() - insets[second as usize].unwrap_or_default();
        if let Some(inset) = insets[end as usize].as_mut() {
            *inset -= (border_extent - view_extent).max(CssPixels::default());
        }
    }
    ResolvedStickyInsets {
        top: insets[Side::Top as usize],
        right: insets[Side::Right as usize],
        bottom: insets[Side::Bottom as usize],
        left: insets[Side::Left as usize],
    }
}

// NB: The computed margins, with auto as zero. The used margins of a block include the space its width leaves free.
fn computed_margins_in_css_pixels(
    layout_arena: &impl PaintableRowsRead,
    slot: NodeSlotId,
    percentage_basis: CssPixels,
) -> FfiPixelBox {
    let Some(style) = layout_arena.node_style_if_live(slot) else {
        return FfiPixelBox::default();
    };
    let resolve = |side: &ComputedLengthPercentageOrAuto| {
        if side.is_auto() {
            CssPixels::from_raw(0)
        } else {
            side.to_px(percentage_basis)
        }
    };
    let margin = &style.surround().margin;
    FfiPixelBox {
        top: resolve(&margin.top),
        right: resolve(&margin.right),
        bottom: resolve(&margin.bottom),
        left: resolve(&margin.left),
    }
}

fn deflated(rect: CssPixelRect, edges: FfiPixelBox) -> CssPixelRect {
    let zero = CssPixels::from_raw(0);
    CssPixelRect::new(
        rect.x + edges.left,
        rect.y + edges.top,
        (rect.width - edges.left - edges.right).max(zero),
        (rect.height - edges.top - edges.bottom).max(zero),
    )
}

fn is_containing_block_descendant_of(
    layout_arena: &impl PaintableRowsRead,
    slot: NodeSlotId,
    ancestor: NodeSlotId,
) -> bool {
    let mut block = layout_arena.paintable_data(slot).containing_block;
    while !block.is_invalid() && layout_arena.paintable_row_is_populated(block) {
        if block == ancestor {
            return true;
        }
        block = layout_arena.paintable_data(block).containing_block;
    }
    false
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
    let mut scroller = scroll_state.state_at_slot(scroller_slot).paintable;
    if !layout_arena.paintable_row_is_populated(paintable) || !layout_arena.paintable_row_is_populated(scroller) {
        return data;
    }

    // NB: Only scroll containers with scrollable overflow get a scroll node. When the nearest scroll container has
    //     none, it sits inside the scroller of the scroll node, and its scrollport never moves. The box sticks to
    //     that scrollport instead.
    if let Some(scroll_container) = nearest_scroll_container_along_containing_blocks(layout_arena, paintable)
        && scroll_container != scroller
        && is_containing_block_descendant_of(layout_arena, scroll_container, scroller)
    {
        scroller = scroll_container;
        data.non_scrolling_scroll_container = scroll_container;
    }

    let sticky_border_box_rect = paintable_geometry::absolute_border_box_rect(layout_arena, paintable);
    let containing_block = layout_arena.paintable_data(paintable).containing_block;
    let scroller_rect = paintable_geometry::absolute_rect(layout_arena, scroller);
    // NB: The box moves within the content box of its containing block, less its own margins. For the scroll
    //     container itself, that is its scrollable overflow rectangle less its padding. This matches other engines.
    let (containing_block_content_box, needs_parent_offset_adjustment) = if containing_block == scroller {
        let overflow_rect = paintable_geometry::scrollable_overflow_rect(layout_arena, containing_block)
            .unwrap_or_else(|| paintable_geometry::absolute_padding_box_rect(layout_arena, containing_block));
        let padding = paintable_geometry::committed_padding(layout_arena, containing_block);
        (deflated(overflow_rect, padding), false)
    } else {
        (paintable_geometry::absolute_rect(layout_arena, containing_block), true)
    };
    let containing_block_width = paintable_geometry::absolute_rect(layout_arena, containing_block).width;
    let containing_block_region = deflated(
        containing_block_content_box.translated(-scroller_rect.x, -scroller_rect.y),
        computed_margins_in_css_pixels(layout_arena, paintable, containing_block_width),
    );

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
    let insets = resolve_sticky_insets_in_css_pixels(
        layout_arena,
        paintable,
        scroller_rect.size(),
        sticky_border_box_rect.size(),
    );
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
    tree: &mut std::sync::Arc<VisualContextTree>,
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
    let tree = std::sync::Arc::make_mut(tree);
    for (node_index, refreshed) in refreshed_sticky_payloads {
        tree.spatial_nodes[node_index.0 as usize].data = SpatialData::Sticky(refreshed);
    }
    true
}

pub(crate) fn refresh_scroll_state(layout_arena: &impl PaintableRowsRead, scroll_state: &mut ScrollState) {
    for slot in 0..scroll_state.slot_count() {
        let state = scroll_state.state_at_slot(slot);
        if state.is_sticky {
            continue;
        }
        let paintable = state.paintable;
        if layout_arena.paintable_row_is_populated(paintable) {
            let offset = layout_arena.row_scroll_offset(paintable);
            scroll_state.set_own_offset(slot, CssPixelPoint::new(-offset.x, -offset.y));
        }
    }
}
