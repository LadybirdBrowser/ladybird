/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a frame is painted with besides the display list and its resources.

use libcompositing_rust::scrolling::{AccumulatedVisualContextTree, ScrollStateSnapshot, SpatialNodeIndex};
use libgfx_rust::{Color, FloatRect, IntRect};

// A scrollbar as it is painted, somewhere between its thin and its expanded geometry.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct PresentedScrollbar {
    pub gutter_rect: FloatRect,
    pub thumb_rect: FloatRect,
    pub thumb_color: Color,
    pub thumb_outline_color: Color,
    pub track_color: Color,
    pub opacity: f32,
}

impl Default for PresentedScrollbar {
    fn default() -> Self {
        Self {
            gutter_rect: Default::default(),
            thumb_rect: Default::default(),
            thumb_color: Default::default(),
            thumb_outline_color: Default::default(),
            track_color: Default::default(),
            opacity: 1.0,
        }
    }
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct ScrollbarOfFrame {
    pub scroll_node_index: SpatialNodeIndex,
    pub vertical: bool,
    pub is_painted_by_compositor: bool,
    pub gutter_rect: FloatRect,
    pub thumb_rect: FloatRect,
    pub expansion: f32,
    pub opacity: f32,
    pub thumb_color: Color,
    pub thumb_outline_color: Color,
    pub track_color: Color,
    pub thin_and_expanded_gutter_rect: IntRect,
}

impl Default for ScrollbarOfFrame {
    fn default() -> Self {
        Self {
            scroll_node_index: Default::default(),
            vertical: false,
            is_painted_by_compositor: false,
            gutter_rect: Default::default(),
            thumb_rect: Default::default(),
            expansion: 0.0,
            opacity: 1.0,
            thumb_color: Default::default(),
            thumb_outline_color: Default::default(),
            track_color: Default::default(),
            thin_and_expanded_gutter_rect: Default::default(),
        }
    }
}

// A scroll offset outside the scroll range moves the thumb past an end of its track, where it is cut short instead.
pub fn thumb_rect_kept_inside_track(mut thumb_rect: IntRect, track_rect: IntRect, vertical: bool) -> IntRect {
    if vertical {
        let top = thumb_rect.y.max(track_rect.y);
        let bottom = thumb_rect.bottom().min(track_rect.bottom());
        thumb_rect.y = top;
        thumb_rect.height = 0.max(bottom - top);
    } else {
        let left = thumb_rect.x.max(track_rect.x);
        let right = thumb_rect.right().min(track_rect.right());
        thumb_rect.x = left;
        thumb_rect.width = 0.max(right - left);
    }
    thumb_rect
}

// What a frame is painted with besides the display list and its resources.
pub struct FrameInputs {
    pub visual_context_tree: AccumulatedVisualContextTree,
    pub presented_scroll_state_snapshot: ScrollStateSnapshot,
    pub scrollbars: Vec<ScrollbarOfFrame>,
    pub viewport_scroll_node_index: Option<SpatialNodeIndex>,
}
