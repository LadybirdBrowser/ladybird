/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libcompositing_rust::scrolling::elastic_overscroll::ElasticOverscroll;
use libcompositing_rust::scrolling::scroll_fling::ScrollVelocityTracker;
use libcompositing_rust::scrolling::{
    AsyncScrollNode, AsyncScrollNodeStableID, AsyncScrollTree, ScrollGesturePhase, ScrollStateSnapshot,
    WheelDeltaPrecision,
};
use libcompositing_rust::time::MonotonicTime;
use libgfx_rust::{FloatPoint, FloatSize};

#[derive(Default)]
pub struct ElasticOverscrollController {
    enabled: bool,
    active_overscrolls: Vec<ActiveOverscroll>,
    step_velocity_tracker: ScrollVelocityTracker,
}

// A scroller that a touchpad gesture holds past an edge of its scroll range, or that is springing back from there.
// The stretch is only presented: it is in no scroll offset that the scroll tree or WebContent sees.
struct ActiveOverscroll {
    stable_node_id: AsyncScrollNodeStableID,
    overscroll: ElasticOverscroll,
}

impl ElasticOverscrollController {
    pub fn set_enabled(&mut self, enabled: bool) {
        self.enabled = enabled;
        if !enabled {
            self.forget();
        }
    }

    pub fn step_can_stretch(
        &self,
        wheel_delta_precision: WheelDeltaPrecision,
        scroll_gesture_phase: ScrollGesturePhase,
    ) -> bool {
        if !self.enabled {
            return false;
        }
        // Only a touchpad gesture says when the fingers lift, which is when a stretch has to spring back.
        if wheel_delta_precision != WheelDeltaPrecision::Precise {
            return false;
        }
        scroll_gesture_phase == ScrollGesturePhase::Ongoing || scroll_gesture_phase == ScrollGesturePhase::Momentum
    }

    pub fn note_step_from_ui(&mut self, delta: FloatPoint, now: MonotonicTime) {
        self.step_velocity_tracker.add_step(delta, now);
    }

    fn active_overscroll_of(&mut self, stable_node_id: AsyncScrollNodeStableID) -> Option<&mut ActiveOverscroll> {
        self.active_overscrolls
            .iter_mut()
            .find(|active| active.stable_node_id == stable_node_id)
    }

    pub fn has_active_overscroll_of(&self, stable_node_id: AsyncScrollNodeStableID) -> bool {
        self.active_overscrolls
            .iter()
            .any(|active| active.stable_node_id == stable_node_id)
    }

    pub fn stretch_of(&self, stable_node_id: AsyncScrollNodeStableID) -> FloatPoint {
        for active in &self.active_overscrolls {
            if active.stable_node_id == stable_node_id {
                return active.overscroll.stretch();
            }
        }
        FloatPoint::default()
    }

    // Uses the part of a finger step that points back to the scroll range to relax the stretch of the scroller, and
    // returns the rest of the step.
    pub fn relax_by(&mut self, stable_node_id: AsyncScrollNodeStableID, step_delta: FloatPoint) -> FloatPoint {
        let Some(active) = self.active_overscroll_of(stable_node_id) else {
            return step_delta;
        };
        let delta_left_to_scroll_with = active.overscroll.relax_by(step_delta);
        if active.overscroll.is_at_rest() {
            self.active_overscrolls
                .retain(|candidate| candidate.stable_node_id != stable_node_id);
        }
        delta_left_to_scroll_with
    }

    // Returns whether the scroller is at an edge that the step points past, which is what lets the step pull it.
    pub fn pull_by(
        &mut self,
        node: &AsyncScrollNode,
        scroll_offset: FloatPoint,
        step_delta: FloatPoint,
        delta_the_scroller_did_not_scroll_by: FloatPoint,
        visual_viewport_magnification: f32,
    ) -> bool {
        // A touchpad step is rarely along one axis exactly. Only its main direction pulls, so that a vertical pan next to
        // a side edge does not creep past that edge.
        let mut overscroll_delta = FloatPoint::default();
        let step_is_mainly_vertical = step_delta.y.abs() >= step_delta.x.abs();
        if step_is_mainly_vertical {
            let delta_y = delta_the_scroller_did_not_scroll_by.y;
            let is_at_the_edge_the_delta_points_past = (delta_y < 0.0 && scroll_offset.y <= node.min_scroll_offset.y)
                || (delta_y > 0.0 && scroll_offset.y >= node.max_scroll_offset.y);
            if node.can_be_wheel_scrolled_vertically
                && node.shows_overscroll_effect_vertically
                && is_at_the_edge_the_delta_points_past
            {
                overscroll_delta.y = delta_y;
            }
        } else {
            let delta_x = delta_the_scroller_did_not_scroll_by.x;
            let is_at_the_edge_the_delta_points_past = (delta_x < 0.0 && scroll_offset.x <= node.min_scroll_offset.x)
                || (delta_x > 0.0 && scroll_offset.x >= node.max_scroll_offset.x);
            if node.can_be_wheel_scrolled_horizontally
                && node.shows_overscroll_effect_horizontally
                && is_at_the_edge_the_delta_points_past
            {
                overscroll_delta.x = delta_x;
            }
        }
        if overscroll_delta.is_zero() {
            return false;
        }

        let active = match self
            .active_overscrolls
            .iter()
            .position(|active| active.stable_node_id == node.stable_node_id)
        {
            Some(index) => &mut self.active_overscrolls[index],
            None => {
                // A zoomed page shows only the part of the viewport's scrollport that its visual viewport is on, and that part
                // is what bounds how far the viewport stretches.
                let mut scrollport_size = FloatSize {
                    width: node.scrollport_rect.width as f32,
                    height: node.scrollport_rect.height as f32,
                };
                if node.is_viewport {
                    scrollport_size.width *= 1.0_f32 / visual_viewport_magnification;
                    scrollport_size.height *= 1.0_f32 / visual_viewport_magnification;
                }
                if scrollport_size.width.is_nan()
                    || scrollport_size.height.is_nan()
                    || scrollport_size.width < 0.0
                    || scrollport_size.height < 0.0
                {
                    return false;
                }
                self.active_overscrolls.push(ActiveOverscroll {
                    stable_node_id: node.stable_node_id,
                    overscroll: ElasticOverscroll::new(scrollport_size, visual_viewport_magnification),
                });
                self.active_overscrolls.last_mut().unwrap()
            }
        };
        active.overscroll.pull_by(overscroll_delta);
        true
    }

    pub fn release_held_by_fingers(&mut self, now: MonotonicTime) {
        let velocity = self.step_velocity_tracker.velocity_at_lift(now).unwrap_or_default();
        for active in &mut self.active_overscrolls {
            active.overscroll.release(velocity, now);
        }
    }

    // A scroller that something else scrolls along an axis it is stretched in lets go of the stretch at once, while one
    // that something else scrolls smoothly springs back from it.
    pub fn drop_stretch_moved_by(&mut self, stable_node_id: AsyncScrollNodeStableID, scroll_delta: FloatPoint) {
        let stretch = self.stretch_of(stable_node_id);
        if (stretch.x != 0.0 && scroll_delta.x != 0.0) || (stretch.y != 0.0 && scroll_delta.y != 0.0) {
            self.active_overscrolls
                .retain(|active| active.stable_node_id != stable_node_id);
        }
    }

    pub fn release_stretch_of(&mut self, stable_node_id: AsyncScrollNodeStableID, now: MonotonicTime) {
        if let Some(active) = self.active_overscroll_of(stable_node_id)
            && !active.overscroll.is_rebounding()
        {
            active.overscroll.release(FloatPoint::default(), now);
        }
    }

    pub fn end_gesture(&mut self, now: MonotonicTime) {
        self.release_held_by_fingers(now);
        self.step_velocity_tracker.reset();
    }

    pub fn forget(&mut self) {
        self.active_overscrolls.clear();
        self.step_velocity_tracker.reset();
    }

    pub fn did_install_scrolling_state(&mut self, async_scroll_tree: &AsyncScrollTree) {
        self.active_overscrolls.retain(|active| {
            let node = async_scroll_tree
                .scroll_node_index_for_stable_id(active.stable_node_id)
                .and_then(|index| async_scroll_tree.scroll_node_for_index(index));
            let Some(node) = node else {
                return false;
            };
            // A scroller that has been told not to stretch along an axis since it was pulled lets go at once.
            let stretch = active.overscroll.stretch();
            !((stretch.x != 0.0 && !node.shows_overscroll_effect_horizontally)
                || (stretch.y != 0.0 && !node.shows_overscroll_effect_vertically))
        });
    }

    pub fn has_rebound(&self) -> bool {
        self.active_overscrolls
            .iter()
            .any(|active| active.overscroll.is_rebounding())
    }

    pub fn advance_rebounds(&mut self, frame_time: MonotonicTime) -> bool {
        let mut a_presented_stretch_changed = false;
        self.active_overscrolls.retain_mut(|active| {
            if !active.overscroll.is_rebounding() {
                return true;
            }
            let stretch_before = active.overscroll.stretch();
            let is_still_rebounding = active.overscroll.advance_rebound_to(frame_time);
            if active.overscroll.stretch() != stretch_before {
                a_presented_stretch_changed = true;
            }
            is_still_rebounding
        });
        a_presented_stretch_changed
    }

    // The scroll offsets to present when a scroller is stretched, which are the only ones that leave a scroll range.
    pub fn stretched_scroll_state_snapshot(
        &self,
        scroll_state_snapshot: &ScrollStateSnapshot,
        async_scroll_tree: &AsyncScrollTree,
    ) -> Option<ScrollStateSnapshot> {
        let mut stretched_scroll_state_snapshot = None;
        for active in &self.active_overscrolls {
            let stretch = active.overscroll.stretch();
            if stretch.is_zero() {
                continue;
            }
            let Some(scroll_node_index) = async_scroll_tree.scroll_node_index_for_stable_id(active.stable_node_id)
            else {
                continue;
            };
            let snapshot = stretched_scroll_state_snapshot.get_or_insert_with(|| scroll_state_snapshot.clone());
            // Only the scroller's own entry moves. The sticky boxes in it keep the shifts resolved for the scroll offset
            // it really has, so they move with its content.
            snapshot.set_device_offset_for_index(
                scroll_node_index,
                snapshot.device_offset_for_index(scroll_node_index) - stretch,
            );
        }
        stretched_scroll_state_snapshot
    }
}
