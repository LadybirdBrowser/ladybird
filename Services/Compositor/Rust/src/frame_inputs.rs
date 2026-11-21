/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a frame is painted with besides the display list and its resources.

use crate::context_scene::ReceivedLayerPlan;
use crate::overlays::OverlayPicture;
use libcompositing_rust::display_list::replay::ScrollbarShownByFrame;
use libcompositing_rust::scrolling::{AccumulatedVisualContextTree, ScrollStateSnapshot, SpatialNodeIndex};
use libcompositing_rust::time::MonotonicTime;
use libgfx_rust::{Color, FloatRect, IntRect};

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

pub use libcompositing_rust::display_list::replay::thumb_rect_kept_inside_track;

pub struct FrameInputs {
    pub visual_context_tree: AccumulatedVisualContextTree,
    /// The plan of layers WebContent sent for the display list and the tree it published. None where it sent none, or
    /// where the frame draws the page through a visual viewport transform other than that of the published tree, as the
    /// plan does not hold for the frame then.
    pub layer_plan: Option<ReceivedLayerPlan>,
    pub presented_scroll_state_snapshot: ScrollStateSnapshot,
    pub scrollbars: Vec<ScrollbarOfFrame>,
    /// When the frame is painted, which is the time a blinking caret shows as of.
    pub painted_at: MonotonicTime,
    /// What the compositor draws over the page in the frame.
    pub overlays: OverlayPicture,
}

impl FrameInputs {
    /// The scrollbars of the page that the frame presents, which replace the geometry the display list recorded for
    /// them.
    pub fn scrollbars_shown_for_the_page(&self) -> Vec<ScrollbarShownByFrame> {
        self.scrollbars
            .iter()
            .filter(|scrollbar| !scrollbar.is_painted_by_compositor)
            .map(|scrollbar| ScrollbarShownByFrame {
                scroll_node_index: scrollbar.scroll_node_index,
                vertical: scrollbar.vertical,
                gutter_rect: scrollbar.gutter_rect,
                thumb_rect: scrollbar.thumb_rect,
                expansion: scrollbar.expansion,
                opacity: scrollbar.opacity,
            })
            .collect()
    }

    fn scroll_offset_changed_since(&self, last_frame_inputs: &FrameInputs, index: SpatialNodeIndex) -> bool {
        last_frame_inputs
            .presented_scroll_state_snapshot
            .device_offset_for_index(index)
            != self.presented_scroll_state_snapshot.device_offset_for_index(index)
    }

    pub fn gutters_of_compositor_painted_scrollbars_that_scrolled_since(
        &self,
        last_frame_inputs: &FrameInputs,
    ) -> IntRect {
        self.scrollbars
            .iter()
            .filter(|scrollbar| {
                scrollbar.is_painted_by_compositor
                    && self.scroll_offset_changed_since(last_frame_inputs, scrollbar.scroll_node_index)
            })
            .fold(IntRect::default(), |damage, scrollbar| {
                damage.united(scrollbar.thin_and_expanded_gutter_rect)
            })
    }
}
