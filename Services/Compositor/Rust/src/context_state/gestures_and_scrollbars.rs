/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Gesture lifetimes, scrollbar scrolling, and visual animations for a compositor context.

use super::*;

impl ContextState {
    pub(super) fn resolve_wheel_scroll_latch(
        &mut self,
        scroll_gesture_phase: ScrollGesturePhase,
        gesture_id: Option<u64>,
    ) -> ResolvedLatch {
        self.wheel_gesture_routing.resolve_latch(
            &self.async_scroll_tree,
            self.scene.visual_context_tree().as_ref(),
            scroll_gesture_phase,
            gesture_id,
        )
    }

    // The scroller the first step of a gesture goes to, as the hit test at the position finds it, or as it is found
    // from the innermost of the scrollers the page found around the target that this context knows.
    pub(super) fn route_first_wheel_step(
        &mut self,
        position: FloatPoint,
        delta: FloatPoint,
        wheel_delta_precision: WheelDeltaPrecision,
        wheel_step_dispatch: WheelStepDispatch,
        nested_context_regions: NestedContextRegions,
        scroll_chain: &[AsyncScrollNodeStableID],
    ) -> WheelHitTestResult {
        let in_flight_destination_for = |state: &Self, stable_node_id| {
            if wheel_delta_precision == WheelDeltaPrecision::Discrete {
                return state.in_flight_wheel_destination(stable_node_id);
            }
            None
        };
        for &stable_node_id in scroll_chain {
            if let Some(scroll_node_index) = self.async_scroll_tree.scroll_node_index_for_stable_id(stable_node_id) {
                return self
                    .async_scroll_tree
                    .hit_test_result_for_scroll_node(scroll_node_index, delta, &|id| {
                        in_flight_destination_for(self, id)
                    });
            }
        }
        let visual_context_tree = self.scene.visual_context_tree_for_compositing().clone();
        self.async_scroll_tree.hit_test_scroll_node_for_wheel(
            &visual_context_tree,
            position,
            delta,
            &|id| in_flight_destination_for(self, id),
            wheel_step_dispatch,
            nested_context_regions,
        )
    }

    // The scroller the first step of a wheel gesture is routed to, which the gesture is latched to.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn route_and_latch_wheel_gesture(
        &mut self,
        position: FloatPoint,
        delta: FloatPoint,
        wheel_delta_precision: WheelDeltaPrecision,
        scroll_gesture_phase: ScrollGesturePhase,
        gesture_id: Option<u64>,
        wheel_step_dispatch: WheelStepDispatch,
        nested_context_regions: NestedContextRegions,
        scroll_chain: &[AsyncScrollNodeStableID],
    ) -> WheelGestureLatchAttempt {
        let scroll_target = self.route_first_wheel_step(
            position,
            delta,
            wheel_delta_precision,
            wheel_step_dispatch,
            nested_context_regions,
            scroll_chain,
        );
        if scroll_target.is_blocked_by_a_region() {
            return WheelGestureLatchAttempt {
                decision: input_routing_decision_for_first_wheel_step(scroll_target.outcome),
                latched_scroll_node_index: None,
            };
        }
        let mut scroll_node_index_to_latch = scroll_target.scroll_node_index;
        let mut decision = InputRoutingDecision::Latched;
        // Fingers that start to pull where every scroller under them is already at its edge stretch the outermost one. A
        // context nested in another one turns the gesture away instead, which passes it on to the document around it.
        let fingers_start_a_gesture_at_an_edge = scroll_node_index_to_latch.is_none()
            && scroll_target.innermost_scroll_node_index.is_some()
            && !delta.is_zero()
            && scroll_gesture_phase == ScrollGesturePhase::Ongoing
            && self.wheel_step_can_stretch_elastically(wheel_delta_precision, scroll_gesture_phase);
        if fingers_start_a_gesture_at_an_edge {
            scroll_node_index_to_latch = self
                .async_scroll_tree
                .outermost_scroll_node_wheel_scrollable_along_main_axis_of(
                    scroll_target.innermost_scroll_node_index.unwrap(),
                    delta,
                );
            decision = InputRoutingDecision::LatchedOutermostScrollerToStretch;
        }
        let Some(index) = scroll_node_index_to_latch else {
            return WheelGestureLatchAttempt {
                decision: input_routing_decision_for_first_wheel_step(scroll_target.outcome),
                latched_scroll_node_index: None,
            };
        };
        let node = self.async_scroll_tree.scroll_node_for_index(index);
        assert!(node.is_some());
        self.wheel_gesture_routing
            .latch(node.unwrap().stable_node_id, scroll_gesture_phase, gesture_id);
        WheelGestureLatchAttempt {
            decision,
            latched_scroll_node_index: scroll_node_index_to_latch,
        }
    }

    pub(super) fn report_started_user_scroll(
        &mut self,
        operation_id: AsyncScrollOperationID,
        stable_node_id: AsyncScrollNodeStableID,
        step_start_offset: FloatPoint,
        destination_offset: FloatPoint,
        animation_kind: ScrollAnimationKind,
        is_absolute_scroll: bool,
    ) {
        let scroll_node_index = self.async_scroll_tree.scroll_node_index_for_stable_id(stable_node_id);
        assert!(scroll_node_index.is_some());
        let current_offset = self
            .async_scroll_tree
            .css_scroll_offset_for_node(scroll_node_index.unwrap(), &self.scroll_state_snapshot);
        assert!(current_offset.is_some());
        let destination = self.async_scroll_tree.css_pixels_from_device_offset(destination_offset);
        self.note_user_input_scrolled(stable_node_id);
        self.async_scroll_update_outbox
            .record_started_user_scroll(StartedUserScroll {
                stable_node_id,
                operation_id,
                initial_scroll_offset: current_offset.unwrap(),
                unsnapped_scroll_destination: destination,
                relative_scroll_delta: {
                    let start = self.async_scroll_tree.css_pixels_from_device_offset(step_start_offset);
                    libcompositing_rust::css_pixels::CssPixelPoint {
                        x: destination.x - start.x,
                        y: destination.y - start.y,
                    }
                },
                selection: SnapDestination {
                    position: destination,
                    ..Default::default()
                },
                settles_gesture: false,
                is_absolute_scroll,
                animation_kind: Some(animation_kind),
            });
    }

    pub(super) fn cancel_smooth_scroll_taken_over_by_user_input(&mut self, scroll_node_index: SpatialNodeIndex) {
        for animation in &self.smooth_scroll_animations {
            let animated_scroll_node_index = self
                .async_scroll_tree
                .scroll_node_index_for_stable_id(animation.stable_node_id);
            if animated_scroll_node_index != Some(scroll_node_index) {
                continue;
            }
            self.async_scroll_update_outbox
                .note_operation_taken_over_by_user_input(animation.operation_id);
            let stable_node_id = animation.stable_node_id;
            self.cancel_smooth_scroll(stable_node_id);
            return;
        }
    }

    pub(super) fn box_of_captured_scrollbar(&self) -> Option<AsyncScrollNodeStableID> {
        let scrollbar_index = self.scrollbar_controller.captured_scrollbar_index()?;
        self.scrollbar_controller
            .scrollbars()
            .get(scrollbar_index)?
            .scroller_stable_node_id
    }

    pub(super) fn note_user_scroll_gesture_end_if_drag_ended(
        &mut self,
        box_of_dragged_scrollbar: Option<AsyncScrollNodeStableID>,
    ) {
        if let Some(id) = box_of_dragged_scrollbar
            && !self.scrollbar_controller.has_captured_scrollbar()
        {
            self.async_scroll_update_outbox.note_user_scroll_gesture_ended(&[id]);
        }
    }

    pub(super) fn end_user_scroll_gesture_of_held_keys(&mut self) {
        self.async_scroll_update_outbox
            .note_user_scroll_gesture_ended(&self.boxes_scrolled_by_held_keys);
        self.boxes_scrolled_by_held_keys.clear();
    }

    pub(super) fn user_scroll_gesture_state(&self) -> UserScrollGestureState {
        let mut state = UserScrollGestureState::default();
        if let Some(id) = self.box_of_captured_scrollbar() {
            state.boxes_in_user_scroll_gesture.push(id);
        }
        for &id in &self.boxes_scrolled_by_held_keys {
            if !state.boxes_in_user_scroll_gesture.contains(&id) {
                state.boxes_in_user_scroll_gesture.push(id);
            }
        }
        for id in self.scroll_snap_controller.boxes_with_gesture_awaiting_input() {
            if !state.boxes_in_user_scroll_gesture.contains(&id) {
                state.boxes_in_user_scroll_gesture.push(id);
            }
        }
        if let Some(id) = self.wheel_gesture_routing.scroller_held_by_fingers()
            && !state.boxes_in_user_scroll_gesture.contains(&id)
        {
            state.boxes_in_user_scroll_gesture.push(id);
        }
        state.wheel_latched_scroller = self.wheel_gesture_routing.latched_scroller();
        state
    }

    pub(super) fn schedule_end_of_scroll_step_gestures(&self, now: MonotonicTime) {
        let deadline = self.scroll_snap_controller.earliest_gesture_input_deadline();
        let Some(deadline) = deadline else {
            self.host
                .cancel_timer(self.context_id, ContextTimer::ScrollStepGestureInput);
            return;
        };
        self.host.set_timer(
            self.context_id,
            ContextTimer::ScrollStepGestureInput,
            max(deadline - now, Duration::zero()),
        );
    }

    // A gesture whose steps this context chains ends once they stop arriving, which is reported to WebContent so
    // that it settles the gesture then rather than on a clock of its own.
    pub(super) fn end_scroll_step_gestures_whose_input_ran_out(&mut self, now: MonotonicTime) {
        let ended_boxes = self.scroll_snap_controller.end_gestures_whose_input_ran_out(now);
        if !ended_boxes.is_empty() {
            self.async_scroll_update_outbox
                .note_user_scroll_gesture_ended(&ended_boxes);
            self.request_rendering_update();
        }
        self.schedule_end_of_scroll_step_gestures(now);
    }

    #[allow(clippy::unnecessary_wraps)]
    fn start_scrollbar_press_scroll(
        &mut self,
        node: &AsyncScrollNode,
        old_scroll_offset: FloatPoint,
        destination: FloatPoint,
        is_absolute_scroll: bool,
        now: MonotonicTime,
    ) -> Option<PendingFrame> {
        let operation_id = self.async_scroll_update_outbox.allocate_operation_id();
        self.smooth_scroll_animations.push(ActiveSmoothScrollAnimation {
            stable_node_id: node.stable_node_id,
            operation_id,
            animation: SmoothScrollAnimation::new(
                old_scroll_offset,
                destination,
                self.async_scroll_tree.device_pixels_per_css_pixel(),
                ScrollAnimationKind::SmoothScroll,
            ),
            started_at: now,
            is_user_scroll: true,
            heads_to_an_extent: false,
        });
        self.report_started_user_scroll(
            operation_id,
            node.stable_node_id,
            old_scroll_offset,
            destination,
            ScrollAnimationKind::SmoothScroll,
            is_absolute_scroll,
        );
        self.scrollbar_press_scroll_operation = Some(operation_id);
        Some(PendingFrame::repainting_changes(self.async_scrolling_viewport_rect))
    }

    // https://source.chromium.org/chromium/chromium/src/+/main:cc/input/scroll_utils.cc
    // INTEROP: A page is most of the scrollport, keeping as much of the last page in view as Chromium does.
    fn scrollbar_page_step(scrollport_length: f32, device_pixels_per_css_pixel: f32) -> f32 {
        const MIN_FRACTION_TO_STEP_WHEN_PAGING: f32 = 0.875;
        const MAX_OVERLAP_BETWEEN_PAGES: f32 = 40.0;
        max(
            1.0,
            max(
                scrollport_length * MIN_FRACTION_TO_STEP_WHEN_PAGING,
                scrollport_length - MAX_OVERLAP_BETWEEN_PAGES * device_pixels_per_css_pixel,
            ),
        )
    }

    // Pages the scroller of the captured scrollbar once toward the pointer, from where a page scroll in flight goes, unless
    // the thumb has reached the pointer there.
    pub(super) fn page_captured_scrollbar(&mut self, now: MonotonicTime) -> Option<PendingFrame> {
        let scrollbar_index = self.scrollbar_controller.captured_scrollbar_index();
        if scrollbar_index.is_none() || self.scrollbar_paging.is_none_or(|paging| paging.direction == 0) {
            return None;
        }
        let scrollbar_index = scrollbar_index.unwrap();
        let scrollbar = *self.scrollbar_controller.scrollbars().get(scrollbar_index)?;
        let node = self
            .async_scroll_tree
            .scroll_node_for_index(scrollbar.scroll_node_index)
            .copied();
        let old_scroll_offset = self
            .async_scroll_tree
            .scroll_offset_for_node(scrollbar.scroll_node_index, &self.scroll_state_snapshot);
        let (Some(node), Some(old_scroll_offset)) = (node, old_scroll_offset) else {
            return None;
        };
        let running_animation = self.running_scrollbar_press_scroll();
        let step_start = running_animation.map_or(old_scroll_offset, |index| {
            self.smooth_scroll_animations[index].animation.destination_offset()
        });
        let paging = self.scrollbar_paging.unwrap();
        if self
            .scrollbar_controller
            .paging_direction(scrollbar_index, paging.primary_position, step_start)
            != paging.direction
        {
            return None;
        }
        let orientation = if scrollbar.vertical {
            Orientation::Vertical
        } else {
            Orientation::Horizontal
        };
        let step = Self::scrollbar_page_step(
            node.scrollport_rect.primary_size_for_orientation(orientation) as f32,
            self.async_scroll_tree.device_pixels_per_css_pixel() as f32,
        );
        let mut destination = step_start;
        destination.set_primary_offset_for_orientation(
            orientation,
            step_start.primary_offset_for_orientation(orientation) + paging.direction as f32 * step,
        );
        destination = self
            .async_scroll_tree
            .clamped_scroll_offset_for_node(scrollbar.scroll_node_index, destination);
        if destination == step_start {
            return None;
        }
        if let Some(index) = running_animation {
            self.retarget_user_scroll(index, destination, now);
            let operation_id = self.smooth_scroll_animations[index].operation_id;
            self.report_started_user_scroll(
                operation_id,
                node.stable_node_id,
                step_start,
                destination,
                ScrollAnimationKind::SmoothScroll,
                false,
            );
            return Some(PendingFrame::repainting_changes(self.async_scrolling_viewport_rect));
        }
        self.cancel_smooth_scroll_taken_over_by_user_input(scrollbar.scroll_node_index);
        self.start_scrollbar_press_scroll(&node, old_scroll_offset, destination, false, now)
    }

    // The smooth scroll that a press beside the thumb started, while it lasts.
    fn running_scrollbar_press_scroll(&mut self) -> Option<usize> {
        let operation_id = self.scrollbar_press_scroll_operation?;
        let animation = self
            .smooth_scroll_animations
            .iter()
            .position(|animation| animation.operation_id == operation_id);
        if animation.is_none() {
            self.scrollbar_press_scroll_operation = None;
            return None;
        }
        animation
    }

    pub(super) fn stop_scrollbar_paging(&mut self) {
        self.scrollbar_paging = None;
        self.host.cancel_timer(self.context_id, ContextTimer::ScrollbarPaging);
        self.scrollbar_paging_timer_is_armed = false;
    }

    pub(super) fn apply_scrollbar_drag(&mut self, drag: &Drag, now: MonotonicTime) -> Option<PendingFrame> {
        let dragged_to = self.scrollbar_controller.scroll_offset_for_drag(
            &self.async_scroll_tree,
            &self.scroll_state_snapshot,
            drag,
        )?;
        // A scrollbar scrolls its own scroller to where the thumb was dragged, and never hands any of that to an ancestor.
        let scroll_node_index = dragged_to.scroll_node_index;
        let node = self.async_scroll_tree.scroll_node_for_index(scroll_node_index).copied();
        let old_scroll_offset = self
            .async_scroll_tree
            .scroll_offset_for_node(scroll_node_index, &self.scroll_state_snapshot);
        let (Some(node), Some(old_scroll_offset)) = (node, old_scroll_offset) else {
            return None;
        };
        let destination = self
            .async_scroll_tree
            .clamped_scroll_offset_for_node(scroll_node_index, dragged_to.scroll_offset);
        // A press beside the thumb scrolls there smoothly, as a page step does. Dragging on before that scroll has ended
        // moves where it goes.
        if let Some(index) = self.running_scrollbar_press_scroll() {
            if self.smooth_scroll_animations[index].animation.destination_offset() == destination {
                return None;
            }
            self.retarget_user_scroll(index, destination, now);
            let operation_id = self.smooth_scroll_animations[index].operation_id;
            self.report_started_user_scroll(
                operation_id,
                node.stable_node_id,
                old_scroll_offset,
                destination,
                ScrollAnimationKind::SmoothScroll,
                true,
            );
            return Some(PendingFrame::repainting_changes(self.async_scrolling_viewport_rect));
        }
        if destination == old_scroll_offset {
            return None;
        }
        self.cancel_smooth_scroll_taken_over_by_user_input(scroll_node_index);
        if drag.is_press_beside_thumb {
            return self.start_scrollbar_press_scroll(&node, old_scroll_offset, destination, true, now);
        }
        let new_scroll_offset = self.async_scroll_tree.set_scroll_offset(
            scroll_node_index,
            dragged_to.scroll_offset,
            self.scene.current_visual_context_tree(),
            &mut self.scroll_state_snapshot,
        );
        self.note_user_input_scrolled(node.stable_node_id);
        assert!(new_scroll_offset.is_some());
        self.rebuild_wheel_hit_test_targets();
        let mut scroll_offsets = Vec::new();
        let new_scroll_offset = new_scroll_offset.unwrap();
        scroll_offsets.push(AsyncScrollOffset {
            stable_node_id: node.stable_node_id,
            compositor_scroll_offset: new_scroll_offset,
            unadopted_scroll_delta: new_scroll_offset - old_scroll_offset,
            last_relative_scroll_delta: Default::default(),
            visual_viewport_scale: None,
            applied_scroll_request_id: 0,
            user_input_scroll_count: 0,
            context_user_input_scroll_count: 0,
        });
        self.store_pending_async_scroll_offsets(&scroll_offsets, None);
        if !node.is_viewport {
            return Some(PendingFrame::repainting_changes(self.async_scrolling_viewport_rect));
        }
        self.async_scrolling_viewport_rect.x = new_scroll_offset.x as i32;
        self.async_scrolling_viewport_rect.y = new_scroll_offset.y as i32;
        Some(PendingFrame::repainting_everything(self.async_scrolling_viewport_rect))
    }

    pub(super) fn rebuild_wheel_hit_test_targets(&mut self) {
        assert!(self.scene.display_list().is_some());
        let display_list = self.scene.display_list().as_ref().unwrap().clone();
        self.async_scroll_tree.rebuild_wheel_hit_test_targets(
            Some(&display_list),
            Some(self.scene.visual_context_tree_for_compositing()),
            &self.scroll_state_snapshot,
        );
    }

    pub fn visual_animations_need_frame(&mut self, now: MonotonicTime) -> bool {
        self.scene
            .visual_animations_need_frame(&self.scroll_state_snapshot, self.viewport_size, now)
    }

    pub fn advance_visual_animations(&mut self, now: MonotonicTime) -> bool {
        let animations_are_active = self.scene.advance_visual_animations(now);
        // The main thread hovers what is under the pointer at rest again as a transform animation moves content under it,
        // with the animation sampled at the frame's time, and once more as the content moves away. So does the clock lane
        // of the frame, at its next tick, while the main thread runs a task.
        let animated_content_is_under_pointer = animations_are_active
            && self.pointer_position.is_some_and(|position| {
                self.scene
                    .visual_context_tree_for_compositing()
                    .running_transform_animation_covers_point(now.nanoseconds(), position, &self.scroll_state_snapshot)
            });
        if animated_content_is_under_pointer || self.animated_content_was_under_pointer {
            self.host.post_to_web_content(
                self.web_content_client_id,
                compositor_web_content_client::messages::AnimatedContentMovedUnderPointer {
                    context_id: self.context_id,
                    frame_time_nanoseconds: now.nanoseconds(),
                },
            );
            self.host.post_to_render_clock_of_web_content(
                self.web_content_client_id,
                render_clock_client::messages::PointerRestsOnMovingContent {
                    context_id: self.context_id,
                },
            );
        }
        self.animated_content_was_under_pointer = animated_content_is_under_pointer;
        animations_are_active
    }
}
