/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a frame is painted with besides the display list and its resources.

use crate::overlays::OverlayPicture;
use libcompositing_rust::display_list::damage::{
    DisplayListDamage, DisplayListFrame, LayerPlans, LiveContentChanges, compute_display_list_damage,
};
use libcompositing_rust::display_list::replay::ScrollbarShownByFrame;
use libcompositing_rust::display_list::storage::DisplayListStorage;
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

// What a frame is painted with besides the display list and its resources.
pub struct FrameInputs {
    pub visual_context_tree: AccumulatedVisualContextTree,
    pub presented_scroll_state_snapshot: ScrollStateSnapshot,
    pub scrollbars: Vec<ScrollbarOfFrame>,
    pub viewport_scroll_node_index: Option<SpatialNodeIndex>,
    // When the frame is painted, which is the time a blinking caret shows as of.
    pub painted_at: MonotonicTime,
    // What the compositor draws over the page in the frame.
    pub overlays: OverlayPicture,
}

impl FrameInputs {
    // The scrollbars of the page that the frame presents, which replace the geometry the display list recorded for
    // them.
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

    // What a frame painted with these inputs repaints of the frame `last_frame_inputs` painted `last_display_list`
    // with, none where that has no bounds, and given the frames' layer plans, what it repaints of each layer.
    pub fn damage_since(
        &self,
        display_list: &DisplayListStorage,
        last_frame_inputs: &FrameInputs,
        last_display_list: &DisplayListStorage,
        viewport_rect: IntRect,
        mut live_content_changes: LiveContentChanges<'_>,
        layer_plans: Option<LayerPlans<'_>>,
    ) -> DisplayListDamage {
        let scroll_offset_changed = |index: SpatialNodeIndex| {
            last_frame_inputs
                .presented_scroll_state_snapshot
                .device_offset_for_index(index)
                != self.presented_scroll_state_snapshot.device_offset_for_index(index)
        };
        if self.viewport_scroll_node_index.is_some_and(scroll_offset_changed) {
            return DisplayListDamage::unbounded(layer_plans.map_or(0, |plans| plans.new.layers.len()));
        }
        for scrollbar in &self.scrollbars {
            if scrollbar.is_painted_by_compositor && scroll_offset_changed(scrollbar.scroll_node_index) {
                live_content_changes.forced_damage_rect = live_content_changes
                    .forced_damage_rect
                    .united(scrollbar.thin_and_expanded_gutter_rect);
            }
        }
        compute_display_list_damage(
            last_frame_inputs.frame_of(last_display_list),
            self.frame_of(display_list),
            viewport_rect,
            live_content_changes,
            layer_plans,
        )
    }

    fn frame_of<'a>(&'a self, display_list: &'a DisplayListStorage) -> DisplayListFrame<'a> {
        DisplayListFrame {
            command_bytes: display_list.bytes(),
            command_runs: display_list.command_runs(),
            summary: display_list.summary(),
            visual_context_tree: self.visual_context_tree.as_arc(),
            scroll_offsets: self.presented_scroll_state_snapshot.device_offsets(),
        }
    }
}
