/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Where a compositor context moves its scrollers.

use super::*;

#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum ScrollerMoveCause {
    WheelStep { scroll_gesture_phase: ScrollGesturePhase },
    ScrollbarThumbDrag,
    ScrollRequest { is_for_script: bool },
    SmoothScrollAnimationTick,
}

impl ScrollerMoveCause {
    fn is_user_input(self) -> bool {
        matches!(self, Self::WheelStep { .. } | Self::ScrollbarThumbDrag)
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum VisualViewportMoveCause {
    Pinch,
    Pan,
    ScrollRequest,
}

pub(super) struct ScrollerMove {
    pub offset_before: FloatPoint,
    pub offset_after: FloatPoint,
}

impl ContextState {
    /// Moves the scroller as far toward the offset as its scroll range lets it. Sticky offsets are left stale for the
    /// caller to resolve once it has moved every scroller it moves.
    pub(super) fn move_scroller(
        &mut self,
        scroll_node_index: SpatialNodeIndex,
        requested_offset: FloatPoint,
        cause: ScrollerMoveCause,
    ) -> Option<ScrollerMove> {
        let node = *self.async_scroll_tree.scroll_node_for_index(scroll_node_index)?;
        let offset_before = AsyncScrollTree::scroll_offset_for_scroll_node(&node, &self.scroll_state_snapshot);
        let offset_after = self
            .async_scroll_tree
            .clamped_scroll_offset_for_node(scroll_node_index, requested_offset);
        if offset_after == offset_before {
            return None;
        }
        self.async_scroll_tree
            .set_scroll_offset_of_node(&node, offset_after, &mut self.scroll_state_snapshot);
        self.sticky_offsets_are_stale = true;
        self.async_scroll_tree.mark_cached_viewport_rects_stale();
        if node.is_viewport {
            self.async_scrolling_viewport_rect.x = offset_after.x as i32;
            self.async_scrolling_viewport_rect.y = offset_after.y as i32;
        }
        if cause.is_user_input() {
            self.note_user_input_scrolled(node.stable_node_id);
        }
        self.scroll_snap_controller.did_move_scroller(node.stable_node_id);
        let scroll_delta = offset_after - offset_before;
        match cause {
            ScrollerMoveCause::WheelStep { scroll_gesture_phase } => {
                // https://drafts.csswg.org/css-scroll-snap-1/#scroll-types
                // A wheel or panning scroll is relative, which scroll-state(scrolled) queries observe.
                self.record_relative_scroll_offset_report(node.stable_node_id, offset_after, scroll_delta);
                self.scroll_snap_controller.did_scroll_node_plainly(
                    node.stable_node_id,
                    scroll_gesture_phase,
                    self.async_scroll_tree.css_pixels_from_device_offset(offset_before),
                );
            }
            ScrollerMoveCause::ScrollRequest { is_for_script } => {
                if is_for_script {
                    self.elastic_overscroll_controller
                        .drop_stretch_moved_by(node.stable_node_id, scroll_delta);
                }
            }
            ScrollerMoveCause::ScrollbarThumbDrag | ScrollerMoveCause::SmoothScrollAnimationTick => {
                self.record_scroll_offset_report(node.stable_node_id, offset_after);
            }
        }
        Some(ScrollerMove {
            offset_before,
            offset_after,
        })
    }

    pub(super) fn move_visual_viewport(
        &mut self,
        document_id: Option<UniqueNodeId>,
        visual_viewport_transform: TransformWithOrigin,
        cause: VisualViewportMoveCause,
    ) {
        self.scene
            .set_async_visual_viewport_transform(visual_viewport_transform);
        self.async_scroll_tree.mark_cached_viewport_rects_stale();
        self.visual_viewport_document_id = document_id;
        let Some(document_id) = document_id else {
            return;
        };
        if cause != VisualViewportMoveCause::ScrollRequest {
            self.note_user_input_scrolled(visual_viewport_stable_node_id(document_id));
        }
        self.record_visual_viewport_offset_report(document_id, &visual_viewport_transform);
    }
}
