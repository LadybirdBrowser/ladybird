/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::scroll_state::{NO_SCROLL_STATE_SLOT, ScrollState, ScrollStateSlot, StickyConstraints};
use crate::css::css_pixels::{CssPixelPoint, CssPixelRect};
use crate::layout::node_data::NodeSlotId;
use crate::painting::host::FfiVisualContextHostCallbacks;
use crate::painting::paintable_geometry;
use crate::painting::paintable_rows::PaintableRowsRead;

pub(crate) fn precompute_sticky_constraints(
    layout_arena: &impl PaintableRowsRead,
    scroll_state: &mut ScrollState,
    sticky_slot: ScrollStateSlot,
    paintable: NodeSlotId,
) {
    let nearest_scrolling_ancestor_slot = scroll_state.nearest_scrolling_ancestor_slot(sticky_slot);
    if nearest_scrolling_ancestor_slot == NO_SCROLL_STATE_SLOT {
        return;
    }
    let scroll_ancestor = scroll_state.state_at_slot(nearest_scrolling_ancestor_slot).paintable;
    if !layout_arena.paintable_row_is_populated(scroll_ancestor) {
        return;
    }
    let sticky_border_box_rect = paintable_geometry::absolute_border_box_rect(layout_arena, paintable);
    let containing_block_of_sticky = layout_arena.paintable_data(paintable).containing_block;
    let scroll_ancestor_rect = paintable_geometry::absolute_rect(layout_arena, scroll_ancestor);

    let (containing_block_region, needs_parent_offset_adjustment) = if containing_block_of_sticky == scroll_ancestor {
        let size = paintable_geometry::scrollable_overflow_rect(layout_arena, containing_block_of_sticky)
            .expect("scroll ancestor has scrollable overflow")
            .size();
        (
            CssPixelRect::from_location_and_size(CssPixelPoint::default(), size),
            false,
        )
    } else {
        let region = paintable_geometry::absolute_border_box_rect(layout_arena, containing_block_of_sticky)
            .translated(-scroll_ancestor_rect.x, -scroll_ancestor_rect.y);
        (region, true)
    };

    let insets = layout_arena.paintable_data(paintable).sticky_insets;
    scroll_state.state_at_slot_mut(sticky_slot).sticky_constraints = Some(StickyConstraints {
        position_relative_to_scroll_ancestor: CssPixelPoint::new(
            sticky_border_box_rect.x - scroll_ancestor_rect.x,
            sticky_border_box_rect.y - scroll_ancestor_rect.y,
        ),
        border_box_size: sticky_border_box_rect.size(),
        scrollport_size: scroll_ancestor_rect.size(),
        containing_block_region,
        needs_parent_offset_adjustment,
        insets,
    });
}

pub(crate) fn refresh_sticky_constraints(layout_arena: &impl PaintableRowsRead, scroll_state: &mut ScrollState) {
    for slot in 0..scroll_state.slot_count() {
        let state = scroll_state.state_at_slot(slot);
        if !state.is_sticky {
            continue;
        }
        // Skip entries whose paintable rows a subtree relayout has replaced; the pending visual
        // context tree rebuild recreates those entries before anything reads them.
        let paintable = state.paintable;
        if layout_arena.paintable_row_is_populated(paintable) {
            precompute_sticky_constraints(layout_arena, scroll_state, slot, paintable);
        }
    }
}

pub(crate) fn refresh_scroll_state(
    layout_arena: &impl PaintableRowsRead,
    callbacks: &FfiVisualContextHostCallbacks,
    scroll_state: &mut ScrollState,
) {
    // https://drafts.csswg.org/css-position/#sticky-pos
    // Sticky positioning is similar to relative positioning except the offsets are automatically calculated
    // in reference to the nearest scrollport.
    // Iteration follows registration order, which is the tree's append order, so parent nodes are
    // always visited before their descendants.
    for slot in 0..scroll_state.slot_count() {
        if !scroll_state.state_at_slot(slot).is_sticky {
            continue;
        }
        let nearest_scrolling_ancestor_slot = scroll_state.nearest_scrolling_ancestor_slot(slot);
        let Some(sticky_data) = scroll_state.state_at_slot(slot).sticky_constraints else {
            continue;
        };
        if nearest_scrolling_ancestor_slot == NO_SCROLL_STATE_SLOT {
            continue;
        }
        let scroll_ancestor = scroll_state.state_at_slot(nearest_scrolling_ancestor_slot).paintable;
        if !layout_arena.paintable_row_is_populated(scroll_ancestor) {
            continue;
        }
        let sticky_insets = sticky_data.insets;

        // For nested sticky elements, work in the coordinate space where the sticky ancestors are
        // at their current positions.
        let parent_sticky_offset = scroll_state.cumulative_sticky_offset(scroll_state.state_at_slot(slot).parent_slot);
        let sticky_position_in_ancestor = sticky_data
            .position_relative_to_scroll_ancestor
            .translated(parent_sticky_offset.x, parent_sticky_offset.y);

        let mut containing_block_region = sticky_data.containing_block_region;
        if sticky_data.needs_parent_offset_adjustment {
            containing_block_region = containing_block_region.translated_by(parent_sticky_offset);
        }
        let min_offset_within_containing_block = containing_block_region.location();
        let max_offset_within_containing_block = CssPixelPoint::new(
            containing_block_region.right() - sticky_data.border_box_size.width,
            containing_block_region.bottom() - sticky_data.border_box_size.height,
        );

        let scroll_ancestor_node = scroll_ancestor;
        let scroll_offset: CssPixelPoint = callbacks
            .scroll_offset(layout_arena.shell_if_live(scroll_ancestor_node))
            .into();
        let scrollport_rect = CssPixelRect::from_location_and_size(scroll_offset, sticky_data.scrollport_size);
        let mut sticky_offset = CssPixelPoint::default();

        if sticky_insets.has_top && scrollport_rect.top() > sticky_position_in_ancestor.y - sticky_insets.top {
            sticky_offset.y = (scrollport_rect.top() + sticky_insets.top).min(max_offset_within_containing_block.y)
                - sticky_position_in_ancestor.y;
        }
        if sticky_insets.has_left && scrollport_rect.left() > sticky_position_in_ancestor.x - sticky_insets.left {
            sticky_offset.x = (scrollport_rect.left() + sticky_insets.left).min(max_offset_within_containing_block.x)
                - sticky_position_in_ancestor.x;
        }
        if sticky_insets.has_bottom
            && scrollport_rect.bottom()
                < sticky_position_in_ancestor.y + sticky_data.border_box_size.height + sticky_insets.bottom
        {
            sticky_offset.y = (scrollport_rect.bottom() - sticky_data.border_box_size.height - sticky_insets.bottom)
                .max(min_offset_within_containing_block.y)
                - sticky_position_in_ancestor.y;
        }
        if sticky_insets.has_right
            && scrollport_rect.right()
                < sticky_position_in_ancestor.x + sticky_data.border_box_size.width + sticky_insets.right
        {
            sticky_offset.x = (scrollport_rect.right() - sticky_data.border_box_size.width - sticky_insets.right)
                .max(min_offset_within_containing_block.x)
                - sticky_position_in_ancestor.x;
        }

        scroll_state.state_at_slot_mut(slot).own_offset = sticky_offset;
    }

    for slot in 0..scroll_state.slot_count() {
        let state = scroll_state.state_at_slot(slot);
        if state.is_sticky {
            continue;
        }
        let paintable = state.paintable;
        if layout_arena.paintable_row_is_populated(paintable) {
            let node = paintable;
            let offset: CssPixelPoint = callbacks.scroll_offset(layout_arena.shell_if_live(node)).into();
            scroll_state.state_at_slot_mut(slot).own_offset = CssPixelPoint::new(-offset.x, -offset.y);
        }
    }
}
