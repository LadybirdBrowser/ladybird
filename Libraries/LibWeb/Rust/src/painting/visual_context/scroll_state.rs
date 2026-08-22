/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::{CssPixelPoint, CssPixelRect, CssPixelSize};
use crate::layout::node_data::NodeSlotId;
use crate::painting::paintable_data::FfiStickyInsets;
use libgfx_rust::FloatPoint;

pub type ScrollStateSlot = usize;
// Position of a scroll or sticky node's entry in the ScrollState store. Slot 0 is the viewport's
// scroll node, so "no slot" needs a value outside the vector.
pub const NO_SCROLL_STATE_SLOT: ScrollStateSlot = usize::MAX;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct StickyConstraints {
    pub position_relative_to_scroll_ancestor: CssPixelPoint,
    pub border_box_size: CssPixelSize,
    pub scrollport_size: CssPixelSize,
    pub containing_block_region: CssPixelRect,
    pub needs_parent_offset_adjustment: bool,
    pub insets: FfiStickyInsets,
}

// Dynamic state of one scroll or sticky node in the accumulated visual context tree. The tree owns
// structure and identity, and the node's ScrollData addresses its entry here by slot; the entry
// carries the values that change without a rebuild: the scroll offset and sticky constraints. The
// scroll-parent reference is derived from the containing block chain at build time, which
// deliberately differs from the node's visual context parent chain for sticky content inside
// fixed-position ancestors.
#[derive(Clone, Debug)]
pub struct ScrollNodeState {
    pub paintable: NodeSlotId,
    pub is_sticky: bool,
    pub node_index: usize,
    pub parent_slot: ScrollStateSlot,
    pub own_offset: CssPixelPoint,
    pub sticky_constraints: Option<StickyConstraints>,
}

// Value store for the scroll and sticky nodes of the accumulated visual context tree: the tree
// owns structure and identity, entries here carry the offsets, sticky constraints, and the
// containing-block-derived scroll-parent references. Registration returns the entry's slot, which
// the tree walk stamps into the node's ScrollData, so resolving a node to its entry is a direct
// index in both directions. Rebuilt together with the tree; offsets are refreshed in place
// between rebuilds.
#[derive(Default)]
pub struct ScrollState {
    pub states: Vec<ScrollNodeState>,
    pub has_non_viewport_wheel_scroll_target_candidate: bool,
}

impl ScrollState {
    pub fn clear(&mut self) {
        self.states.clear();
        self.has_non_viewport_wheel_scroll_target_candidate = false;
    }

    pub fn slot_count(&self) -> usize {
        self.states.len()
    }

    fn append(&mut self, state: ScrollNodeState) -> ScrollStateSlot {
        self.states.push(state);
        self.states.len() - 1
    }

    pub fn register_scroll_node(
        &mut self,
        node_index: usize,
        paintable: NodeSlotId,
        parent_slot: ScrollStateSlot,
    ) -> ScrollStateSlot {
        self.append(ScrollNodeState {
            paintable,
            is_sticky: false,
            node_index,
            parent_slot,
            own_offset: CssPixelPoint::default(),
            sticky_constraints: None,
        })
    }

    pub fn register_sticky_node(
        &mut self,
        node_index: usize,
        paintable: NodeSlotId,
        parent_slot: ScrollStateSlot,
    ) -> ScrollStateSlot {
        self.append(ScrollNodeState {
            paintable,
            is_sticky: true,
            node_index,
            parent_slot,
            own_offset: CssPixelPoint::default(),
            sticky_constraints: None,
        })
    }

    pub fn state_at_slot(&self, slot: ScrollStateSlot) -> &ScrollNodeState {
        &self.states[slot]
    }

    pub fn state_at_slot_mut(&mut self, slot: ScrollStateSlot) -> &mut ScrollNodeState {
        &mut self.states[slot]
    }

    pub fn node_index_for_slot(&self, slot: ScrollStateSlot) -> usize {
        if slot == NO_SCROLL_STATE_SLOT {
            return 0;
        }
        self.states[slot].node_index
    }

    pub fn cumulative_offset(&self, mut slot: ScrollStateSlot) -> CssPixelPoint {
        let mut offset = CssPixelPoint::default();
        while slot != NO_SCROLL_STATE_SLOT {
            let state = &self.states[slot];
            offset = offset.translated(state.own_offset.x, state.own_offset.y);
            slot = state.parent_slot;
        }
        offset
    }

    pub fn cumulative_sticky_offset(&self, mut slot: ScrollStateSlot) -> CssPixelPoint {
        let mut offset = CssPixelPoint::default();
        while slot != NO_SCROLL_STATE_SLOT {
            let state = &self.states[slot];
            if !state.is_sticky {
                break;
            }
            offset = offset.translated(state.own_offset.x, state.own_offset.y);
            slot = state.parent_slot;
        }
        offset
    }

    pub fn nearest_scrolling_ancestor_slot(&self, slot: ScrollStateSlot) -> ScrollStateSlot {
        let mut ancestor = self.states[slot].parent_slot;
        while ancestor != NO_SCROLL_STATE_SLOT {
            let state = &self.states[ancestor];
            if !state.is_sticky {
                return ancestor;
            }
            ancestor = state.parent_slot;
        }
        NO_SCROLL_STATE_SLOT
    }

    pub fn snapshot(&self, device_pixels_per_css_pixel: f64) -> Vec<FloatPoint> {
        let scale = device_pixels_per_css_pixel as f32;
        let mut snapshot: Vec<FloatPoint> = Vec::new();
        for state in &self.states {
            if snapshot.len() <= state.node_index {
                snapshot.resize(state.node_index + 1, FloatPoint::default());
            }
            snapshot[state.node_index] = FloatPoint {
                x: state.own_offset.x.to_float() * scale,
                y: state.own_offset.y.to_float() * scale,
            };
        }
        snapshot
    }
}
