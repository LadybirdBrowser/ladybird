/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Scroll requests and animations for a compositor context.

use super::*;
use libgfx_rust::{scale_matrix, translation_matrix};

impl ContextState {
    pub fn handle_pinch_event(&mut self, event: &PinchEvent) -> ContextUpdateResult {
        if !self.presents_to_client() || !self.has_async_scrolling_state {
            return Default::default();
        }
        if self.scene.visual_context_tree().is_none() {
            return Default::default();
        }
        // NB: A step with nothing to zoom, as at a limit of the scale, is the compositor's all the same: the main thread,
        //     whose scale may lag behind, must not zoom for it.
        let scale = 1.0 + event.scale_delta;
        if scale == 1.0 {
            return ContextUpdateResult {
                accepted: true,
                ..Default::default()
            };
        }
        let mut transform = self.scene.visual_viewport_transform();
        let old_scale = transform.matrix.elements[0][0];
        if old_scale <= 0.0 {
            return Default::default();
        }
        let new_scale = clamp(old_scale as f64 * scale, 1.0, 5.0);
        let applied_scale = (new_scale / old_scale as f64) as f32;
        if applied_scale == 1.0 {
            return ContextUpdateResult {
                accepted: true,
                ..Default::default()
            };
        }
        let position = FloatPoint {
            x: event.position.x as f32,
            y: event.position.y as f32,
        };
        let gesture_transform = translation_matrix(position.x, position.y, 0.0)
            .multiplied(scale_matrix(applied_scale, applied_scale, 1.0))
            .multiplied(translation_matrix(-position.x, -position.y, 0.0));
        let old_scroll_offset = visual_viewport_scroll_offset(&transform);
        transform.matrix = gesture_transform.multiplied(transform.matrix);
        clamp_visual_viewport_transform_to_viewport(&mut transform, self.async_scrolling_viewport_rect);
        self.scene.set_async_visual_viewport_transform(transform);
        self.rebuild_wheel_hit_test_targets();
        self.visual_viewport_document_id = self.async_scroll_tree.document_id();
        if let Some(document_id) = self.async_scroll_tree.document_id() {
            self.note_user_input_scrolled(AsyncScrollNodeStableID {
                node_id: document_id,
                kind: AsyncScrollNodeKind::VisualViewport,
                pseudo_element_type: 0,
            });
            let new_scroll_offset = visual_viewport_scroll_offset(&transform);
            let scroll_offset = AsyncScrollOffset {
                stable_node_id: AsyncScrollNodeStableID {
                    node_id: document_id,
                    kind: AsyncScrollNodeKind::VisualViewport,
                    pseudo_element_type: 0,
                },
                compositor_scroll_offset: new_scroll_offset,
                unadopted_scroll_delta: new_scroll_offset - old_scroll_offset,
                last_relative_scroll_delta: Default::default(),
                visual_viewport_scale: Some(transform.matrix.elements[0][0]),
                applied_scroll_request_id: 0,
                user_input_scroll_count: 0,
                context_user_input_scroll_count: 0,
            };
            self.store_pending_async_scroll_offsets(&[scroll_offset], None);
        }
        ContextUpdateResult {
            accepted: true,
            frame_to_present: Some(PendingFrame::repainting_everything(self.async_scrolling_viewport_rect)),
            should_request_rendering_update: false,
            ..Default::default()
        }
    }

    pub(super) fn start_snap_scroll(
        &mut self,
        scroll_node_index: SpatialNodeIndex,
        snap_scroll: SnapScrollStart,
        settles_gesture: bool,
        now: MonotonicTime,
    ) -> AsyncScrollOperationID {
        let stable_node_id = snap_scroll.stable_node_id;
        // A snap scroll of this context's own that is still running is replaced by the new one; any other scroll of the
        // box is taken over by the input.
        let running_animation = self
            .smooth_scroll_animations
            .iter()
            .find(|animation| animation.stable_node_id == stable_node_id);
        if let Some(running_animation) = running_animation {
            if !self
                .scroll_snap_controller
                .is_snap_scroll(stable_node_id, running_animation.operation_id)
            {
                self.async_scroll_update_outbox
                    .note_operation_taken_over_by_user_input(running_animation.operation_id);
            }
            // Keep the accumulation state the controller just recorded when selecting this snap destination.
            self.retire_smooth_scroll_animation(stable_node_id);
        }
        let current_offset = self
            .async_scroll_tree
            .scroll_offset_for_node(scroll_node_index, &self.scroll_state_snapshot);
        assert!(current_offset.is_some());
        let destination = snap_scroll.selection.position;
        let operation_id = self.async_scroll_update_outbox.allocate_operation_id();
        self.smooth_scroll_animations.push(ActiveSmoothScrollAnimation {
            stable_node_id,
            operation_id,
            animation: SmoothScrollAnimation::new(
                current_offset.unwrap(),
                self.async_scroll_tree.device_offset_from_css_pixels(destination),
                self.async_scroll_tree.device_pixels_per_css_pixel(),
                snap_scroll.animation_kind,
            ),
            started_at: now,
            is_user_scroll: true,
            heads_to_an_extent: false,
        });
        self.scroll_snap_controller
            .did_start_snap_scroll(stable_node_id, operation_id, destination);
        self.note_user_input_scrolled(stable_node_id);
        self.async_scroll_update_outbox
            .record_started_user_scroll(StartedUserScroll {
                stable_node_id,
                operation_id,
                initial_scroll_offset: snap_scroll.initial_scroll_offset,
                unsnapped_scroll_destination: snap_scroll.unsnapped_scroll_destination,
                relative_scroll_delta: snap_scroll.relative_scroll_delta,
                selection: snap_scroll.selection,
                settles_gesture,
                is_absolute_scroll: snap_scroll.is_absolute_scroll,
                animation_kind: Some(snap_scroll.animation_kind),
            });
        operation_id
    }

    // The end of a gesture snaps the boxes it panned; the first snap scroll it starts is the operation a caller follows.
    pub(super) fn snap_at_gesture_end(&mut self, now: MonotonicTime) -> Option<AsyncScrollOperationID> {
        let mut operation_id = None;
        for snap in self
            .scroll_snap_controller
            .decide_gesture_end(&self.async_scroll_tree, &self.scroll_state_snapshot)
        {
            let started_operation_id = self.start_snap_scroll(snap.scroll_node_index, snap.snap_scroll, true, now);
            if operation_id.is_none() {
                operation_id = Some(started_operation_id);
            }
        }
        operation_id
    }

    // The viewport rect a wheel scroll presents, moved along with the viewport when the scroll moved it.
    fn note_async_scrolling_viewport_rect(
        &mut self,
        mut viewport_rect: IntRect,
        scroll_offset: Option<&AsyncScrollOffset>,
    ) -> IntRect {
        if let Some(scroll_offset) = scroll_offset {
            let scroll_node_index = self
                .async_scroll_tree
                .scroll_node_index_for_stable_id(scroll_offset.stable_node_id);
            if scroll_node_index.is_some_and(|index| self.async_scroll_tree.scroll_node_is_viewport(index)) {
                viewport_rect.x = scroll_offset.compositor_scroll_offset.x as i32;
                viewport_rect.y = scroll_offset.compositor_scroll_offset.y as i32;
            }
        }
        self.async_scrolling_viewport_rect = viewport_rect;
        viewport_rect
    }

    // A step of a wheel gesture that is latched to the node. A stretch of the node is relaxed before the visual
    // viewport is panned, the node only scrolls by what both leave of the step, and what the node cannot scroll by
    // either is what pulls it past its edge.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn perform_wheel_scroll_of_node(
        &mut self,
        scroll_node_index: SpatialNodeIndex,
        delta: FloatPoint,
        wheel_delta_precision: WheelDeltaPrecision,
        scroll_gesture_phase: ScrollGesturePhase,
        operation_tracking: AsyncScrollOperationTracking,
        viewport_rect: IntRect,
        scroll_chaining: ScrollChaining,
        visual_viewport_panning: VisualViewportPanning,
        now: MonotonicTime,
    ) -> WheelScrollOutcome {
        let pans_visual_viewport = visual_viewport_panning == VisualViewportPanning::BeforeScrolling;
        let magnification = if pans_visual_viewport {
            self.scene.visual_viewport_magnification()
        } else {
            1.0
        };
        let step_delta = delta.scaled(1.0 / magnification);
        let latched_node = self.async_scroll_tree.scroll_node_for_index(scroll_node_index).copied();
        let step_can_stretch_elastically =
            self.wheel_step_can_stretch_elastically(wheel_delta_precision, scroll_gesture_phase);
        let fingers_can_stretch_latched_scroller = step_can_stretch_elastically
            && scroll_gesture_phase == ScrollGesturePhase::Ongoing
            && latched_node.is_some();
        let mut momentum_can_bounce_latched_scroller = step_can_stretch_elastically
            && scroll_gesture_phase == ScrollGesturePhase::Momentum
            && latched_node.is_some()
            && self
                .wheel_gesture_routing
                .momentum_of_latched_gesture_can_spring_scroller_back();
        let presented_stretch_of_latched_scroller = |controller: &ElasticOverscrollController| {
            latched_node.map_or(FloatPoint::default(), |node| controller.stretch_of(node.stable_node_id))
        };
        let stretch_before_step = presented_stretch_of_latched_scroller(&self.elastic_overscroll_controller);
        // Momentum only arrives once the fingers have lifted, and its first step can be the first sign that they did.
        if step_can_stretch_elastically && scroll_gesture_phase == ScrollGesturePhase::Momentum {
            self.elastic_overscroll_controller.release_held_by_fingers(now);
        }
        if momentum_can_bounce_latched_scroller
            && self
                .elastic_overscroll_controller
                .has_active_overscroll_of(latched_node.as_ref().unwrap().stable_node_id)
        {
            // The fingers lifted from a stretched scroller, which this momentum already springs back with.
            self.wheel_gesture_routing
                .set_momentum_of_latched_gesture_has_sprung_scroller_back(true);
            momentum_can_bounce_latched_scroller = false;
        }
        // A stretched scroller is past the edge of everything the gesture moves, so the fingers relax the stretch before
        // they pan the visual viewport away from its own edge.
        let mut delta_left = delta;
        if fingers_can_stretch_latched_scroller {
            self.wheel_gesture_routing
                .set_momentum_of_latched_gesture_has_sprung_scroller_back(false);
            let step_delta_left = self
                .elastic_overscroll_controller
                .relax_by(latched_node.as_ref().unwrap().stable_node_id, step_delta);
            if step_delta_left != step_delta {
                delta_left = step_delta_left.scaled(magnification);
            }
        }
        let mut outcome = WheelScrollOutcome::default();
        let mut visual_viewport_took_rest_of_step = false;
        if pans_visual_viewport {
            let panned_delta = self.pan_visual_viewport_by(delta_left);
            if !panned_delta.is_zero() {
                delta_left = delta_left - panned_delta;
                visual_viewport_took_rest_of_step = delta_left.is_zero();
                outcome.viewport_rect_to_present = Some(self.note_async_scrolling_viewport_rect(viewport_rect, None));
            }
        }
        let mut delta_the_scroller_did_not_scroll_by = None;
        if !visual_viewport_took_rest_of_step {
            let node_scroll = self.scroll_node_by_wheel_delta(
                scroll_node_index,
                delta_left.scaled(1.0 / magnification),
                wheel_delta_precision,
                scroll_gesture_phase,
                operation_tracking,
                viewport_rect,
                scroll_chaining,
                now,
            );
            outcome.operation_id = node_scroll.outcome.operation_id;
            if node_scroll.outcome.viewport_rect_to_present.is_some() {
                outcome.viewport_rect_to_present = node_scroll.outcome.viewport_rect_to_present;
            }
            delta_the_scroller_did_not_scroll_by = node_scroll.delta_the_node_did_not_scroll_by;
        }
        if let Some(delta_left) = delta_the_scroller_did_not_scroll_by {
            if fingers_can_stretch_latched_scroller {
                self.pull_elastic_overscroll_by(latched_node.as_ref().unwrap(), step_delta, delta_left);
            } else if momentum_can_bounce_latched_scroller
                && self.pull_elastic_overscroll_by(latched_node.as_ref().unwrap(), step_delta, delta_left)
            {
                // Momentum that carries a scroller into its edge bounces it: the velocity it arrives with takes the
                // rebound out past the edge before the rebound brings it back.
                self.elastic_overscroll_controller.release_held_by_fingers(now);
                self.wheel_gesture_routing
                    .set_momentum_of_latched_gesture_has_sprung_scroller_back(true);
            }
        }
        if outcome.viewport_rect_to_present.is_none()
            && presented_stretch_of_latched_scroller(&self.elastic_overscroll_controller) != stretch_before_step
        {
            outcome.viewport_rect_to_present = Some(self.note_async_scrolling_viewport_rect(viewport_rect, None));
            outcome.only_elastic_overscroll_changed = true;
        }
        outcome
    }

    #[allow(clippy::too_many_arguments)]
    pub(super) fn scroll_node_by_wheel_delta(
        &mut self,
        mut scroll_node_index: SpatialNodeIndex,
        delta: FloatPoint,
        wheel_delta_precision: WheelDeltaPrecision,
        scroll_gesture_phase: ScrollGesturePhase,
        operation_tracking: AsyncScrollOperationTracking,
        viewport_rect: IntRect,
        scroll_chaining: ScrollChaining,
        now: MonotonicTime,
    ) -> NodeScrollByWheelDelta {
        let mut outcome = WheelScrollOutcome::default();
        let mut wheel_scroll_target = None;
        if wheel_delta_precision == WheelDeltaPrecision::Discrete {
            wheel_scroll_target = self.async_scroll_tree.select_wheel_scroll_target(
                scroll_node_index,
                delta,
                &self.scroll_state_snapshot,
                &|stable_node_id| self.in_flight_wheel_destination(stable_node_id),
                scroll_chaining,
            );
            if let Some(target) = &wheel_scroll_target {
                let Some(index) = self
                    .async_scroll_tree
                    .scroll_node_index_for_stable_id(target.stable_node_id)
                else {
                    return NodeScrollByWheelDelta {
                        outcome,
                        delta_the_node_did_not_scroll_by: None,
                    };
                };
                scroll_node_index = index;
                self.wheel_gesture_routing.relatch_to(target.stable_node_id);
            }
        }
        let css_delta = self.async_scroll_tree.css_pixels_from_device_offset(delta);
        let mut decision = None;
        if wheel_delta_precision == WheelDeltaPrecision::Discrete {
            let mut scroll_in_flight_destination = None;
            if let Some(node) = self.async_scroll_tree.scroll_node_for_index(scroll_node_index) {
                scroll_in_flight_destination = self
                    .in_flight_wheel_destination(node.stable_node_id)
                    .map(|offset| self.async_scroll_tree.css_pixels_from_device_offset(offset));
            }
            decision = self.scroll_snap_controller.decide_discrete_step(
                &self.async_scroll_tree,
                &self.scroll_state_snapshot,
                scroll_node_index,
                css_delta,
                scroll_in_flight_destination,
                now,
            );
        } else if scroll_gesture_phase == ScrollGesturePhase::Momentum {
            decision = self.scroll_snap_controller.decide_momentum_delta(
                &self.async_scroll_tree,
                &self.scroll_state_snapshot,
                scroll_node_index,
                css_delta,
            );
        }
        if let Some(decision) = decision {
            self.schedule_end_of_scroll_step_gestures(now);
            if let StepDecision::SnapScrollStart(mut snap_scroll) = decision {
                if wheel_delta_precision == WheelDeltaPrecision::Discrete {
                    snap_scroll.animation_kind = ScrollAnimationKind::Wheel;
                }
                let operation_id = self.start_snap_scroll(scroll_node_index, snap_scroll, false, now);
                if operation_tracking == AsyncScrollOperationTracking::Yes {
                    outcome.operation_id = Some(operation_id);
                }
                outcome.viewport_rect_to_present = Some(self.note_async_scrolling_viewport_rect(viewport_rect, None));
                return NodeScrollByWheelDelta {
                    outcome,
                    delta_the_node_did_not_scroll_by: None,
                };
            }
            if operation_tracking == AsyncScrollOperationTracking::Yes {
                outcome.operation_id = Some(self.async_scroll_update_outbox.allocate_operation_id());
                self.async_scroll_update_outbox
                    .complete_operation(outcome.operation_id.unwrap());
            }
            return NodeScrollByWheelDelta {
                outcome,
                delta_the_node_did_not_scroll_by: None,
            };
        }
        if wheel_delta_precision == WheelDeltaPrecision::Discrete {
            let result = self.smooth_wheel_scroll_by(wheel_scroll_target.as_ref(), viewport_rect, now);
            outcome.operation_id = result.enqueue_result.operation_id;
            if result.frame_to_present.is_some() {
                outcome.viewport_rect_to_present = Some(viewport_rect);
            }
            return NodeScrollByWheelDelta {
                outcome,
                delta_the_node_did_not_scroll_by: None,
            };
        }
        self.cancel_smooth_scroll_taken_over_by_user_input(scroll_node_index);
        if operation_tracking == AsyncScrollOperationTracking::Yes {
            outcome.operation_id = Some(self.async_scroll_update_outbox.allocate_operation_id());
        }
        let scroll_offset = self.async_scroll_tree.apply_scroll_delta(
            scroll_node_index,
            delta,
            self.scene.current_visual_context_tree(),
            &mut self.scroll_state_snapshot,
            scroll_chaining,
        );
        let Some(mut scroll_offset) = scroll_offset else {
            if let Some(id) = outcome.operation_id {
                self.async_scroll_update_outbox.complete_operation(id);
            }
            return NodeScrollByWheelDelta {
                outcome,
                delta_the_node_did_not_scroll_by: Some(delta),
            };
        };
        self.note_user_input_scrolled(scroll_offset.stable_node_id);
        // On the first step of a gesture, scroll node chaining may have scrolled an ancestor instead, which took the delta.
        let node = self.async_scroll_tree.scroll_node_for_index(scroll_node_index);
        let an_ancestor_scrolled = node.is_some_and(|node| scroll_offset.stable_node_id != node.stable_node_id);
        let mut delta_the_node_did_not_scroll_by = None;
        if !an_ancestor_scrolled {
            delta_the_node_did_not_scroll_by = Some(delta - scroll_offset.unadopted_scroll_delta);
        }
        // The box the delta moved is the one a gesture pans; on the first step of a gesture, scroll node chaining may have
        // moved an ancestor of the node the gesture latched to.
        let scroll_offset_before_scroll = scroll_offset.compositor_scroll_offset - scroll_offset.unadopted_scroll_delta;
        self.scroll_snap_controller.did_scroll_node_plainly(
            scroll_offset.stable_node_id,
            scroll_gesture_phase,
            self.async_scroll_tree
                .css_pixels_from_device_offset(scroll_offset_before_scroll),
        );
        // https://drafts.csswg.org/css-scroll-snap-1/#scroll-types
        // A wheel or panning scroll is relative, which scroll-state(scrolled) queries observe.
        scroll_offset.last_relative_scroll_delta = scroll_offset.unadopted_scroll_delta;
        self.rebuild_wheel_hit_test_targets();
        self.store_pending_async_scroll_offsets(std::slice::from_ref(&scroll_offset), outcome.operation_id);
        outcome.viewport_rect_to_present =
            Some(self.note_async_scrolling_viewport_rect(viewport_rect, Some(&scroll_offset)));
        NodeScrollByWheelDelta {
            outcome,
            delta_the_node_did_not_scroll_by,
        }
    }

    pub(super) fn note_user_input_scrolled(&mut self, stable_node_id: AsyncScrollNodeStableID) {
        let count = self.user_input_scroll_counts.entry(stable_node_id).or_default();
        *count = count.wrapping_add(1);
        self.context_user_input_scroll_count = self.context_user_input_scroll_count.wrapping_add(1);
    }

    fn axis_held_by_scrollbar_of(&self, scroll_node_index: SpatialNodeIndex) -> Option<Orientation> {
        // Paging steps toward the pointer like keys do, and takes a script's scroll along like them.
        let scrollbar_index = self.scrollbar_controller.captured_scrollbar_index();
        if scrollbar_index.is_none() || self.scrollbar_paging.is_some() {
            return None;
        }
        let scrollbar = self.scrollbar_controller.scrollbars().get(scrollbar_index.unwrap())?;
        if scrollbar.scroll_node_index != scroll_node_index {
            return None;
        }
        Some(if scrollbar.vertical {
            Orientation::Vertical
        } else {
            Orientation::Horizontal
        })
    }

    pub fn apply_scroll_requests(
        &mut self,
        scroll_requests: &[ScrollRequest],
        now: MonotonicTime,
    ) -> Option<PendingFrame> {
        let mut answers = Vec::new();
        for scroll_request in scroll_requests {
            // The box's offsets are reported as of this request from here on, whether or not the box is here yet.
            self.applied_scroll_request_ids
                .insert(scroll_request.stable_node_id, scroll_request.request_id);
            let mut moved_the_box = false;
            let note_request_applied = |state: &mut Self, moved_the_box| {
                state
                    .async_scroll_update_outbox
                    .note_scroll_request_applied(AppliedScrollRequest {
                        stable_node_id: scroll_request.stable_node_id,
                        request_id: scroll_request.request_id,
                        moved_the_box,
                    });
            };
            if scroll_request.stable_node_id.kind == AsyncScrollNodeKind::VisualViewport {
                moved_the_box = self.apply_visual_viewport_scroll_request(scroll_request, &mut answers);
                note_request_applied(self, moved_the_box);
                continue;
            }
            let mut scroll_node_index = None;
            if self.has_async_scrolling_state {
                scroll_node_index = self
                    .async_scroll_tree
                    .scroll_node_index_for_stable_id(scroll_request.stable_node_id);
            }
            let old_offset = scroll_node_index.and_then(|index| {
                self.async_scroll_tree
                    .scroll_offset_for_node(index, &self.scroll_state_snapshot)
            });
            let Some(old_offset) = old_offset else {
                if let Some(smooth_scroll) = &scroll_request.smooth_scroll {
                    self.async_scroll_update_outbox
                        .complete_operation(smooth_scroll.operation_id);
                }
                note_request_applied(self, moved_the_box);
                continue;
            };
            let scroll_node_index = scroll_node_index.unwrap();
            // The answer tells the main thread where the box went for the request, even where it stayed where it was.
            let answer_with_the_offset_of_the_box =
                |state: &mut Self, answers: &mut Vec<AsyncScrollOffset>, moved_the_box: &mut bool| {
                    let new_offset = state
                        .async_scroll_tree
                        .scroll_offset_for_node(scroll_node_index, &state.scroll_state_snapshot)
                        .unwrap();
                    *moved_the_box = new_offset != old_offset;
                    if state.async_scroll_tree.scroll_node_is_viewport(scroll_node_index) {
                        state.async_scrolling_viewport_rect.x = new_offset.x as i32;
                        state.async_scrolling_viewport_rect.y = new_offset.y as i32;
                    }
                    answers.push(AsyncScrollOffset {
                        stable_node_id: scroll_request.stable_node_id,
                        compositor_scroll_offset: new_offset,
                        unadopted_scroll_delta: new_offset - old_offset,
                        last_relative_scroll_delta: Default::default(),
                        visual_viewport_scale: None,
                        applied_scroll_request_id: 0,
                        user_input_scroll_count: 0,
                        context_user_input_scroll_count: 0,
                    });
                };
            // A request aborts the box's smooth scroll, unless it heads that very scroll somewhere else.
            let mut retargeted_operation_id = None;
            if let Some(smooth_scroll) = &scroll_request.smooth_scroll
                && smooth_scroll.retargets_operation
            {
                retargeted_operation_id = Some(smooth_scroll.operation_id);
            }
            let running_animation = self
                .smooth_scroll_animations
                .iter()
                .position(|animation| animation.stable_node_id == scroll_request.stable_node_id);
            let retargets_running_animation = running_animation.is_some_and(|index| {
                Some(self.smooth_scroll_animations[index].operation_id) == retargeted_operation_id
            });
            // A step of the user's by a distance goes on from where the user's scroll of the same kind is headed, unless the
            // box jumped or scrolled elsewhere first.
            let mut destination_of_running_user_scroll = None;
            if let Some(index) = running_animation
                && let Some(smooth_scroll) = &scroll_request.smooth_scroll
                && self.smooth_scroll_animations[index].is_user_scroll
                && self.smooth_scroll_animations[index].animation.kind() == smooth_scroll.animation_kind
                && scroll_request.jump_offset.is_none()
                && scroll_request.relative_scroll.is_none()
            {
                destination_of_running_user_scroll =
                    Some(self.smooth_scroll_animations[index].animation.destination_offset());
            }
            // The main thread settled the user's gesture once it heard of no more input for the box. User input this
            // context took since then carries the gesture on, and the box goes where that input takes it instead.
            let mut smooth_scroll = scroll_request.smooth_scroll.as_ref();
            if let Some(scroll) = smooth_scroll
                && let Some(settled_count) = scroll.settled_user_input_scroll_count
                && !retargets_running_animation
                && self
                    .user_input_scroll_counts
                    .get(&scroll_request.stable_node_id)
                    .copied()
                    .unwrap_or(0)
                    > settled_count
            {
                self.async_scroll_update_outbox
                    .note_operation_taken_over_by_user_input(scroll.operation_id);
                self.async_scroll_update_outbox.complete_operation(scroll.operation_id);
                smooth_scroll = None;
                if scroll_request.jump_offset.is_none() && scroll_request.relative_scroll.is_none() {
                    answer_with_the_offset_of_the_box(self, &mut answers, &mut moved_the_box);
                    note_request_applied(self, moved_the_box);
                    continue;
                }
            }
            let has_instant_scroll = scroll_request.jump_offset.is_some() || scroll_request.relative_scroll.is_some();
            let instant_scroll_is_for_script = has_instant_scroll && !scroll_request.instant_scroll_is_for_user_input;
            let mut smooth_scroll_is_for_script =
                smooth_scroll.is_some_and(|scroll| scroll.initiator == SmoothScrollInitiator::Programmatic);
            // The thumb of a scrollbar that the user holds stays under the pointer: the user scrolls the box along that axis
            // from the moment a script does, which aborts a smooth scroll of the script's.
            let axis_held_by_scrollbar = self.axis_held_by_scrollbar_of(scroll_node_index);
            if axis_held_by_scrollbar.is_some() && smooth_scroll_is_for_script {
                let scroll = smooth_scroll.unwrap();
                self.async_scroll_update_outbox
                    .note_operation_taken_over_by_user_input(scroll.operation_id);
                self.async_scroll_update_outbox.complete_operation(scroll.operation_id);
                smooth_scroll = None;
                smooth_scroll_is_for_script = false;
            }
            // A smooth scroll in progress is aborted by any other, and by an instant scroll, except that a script's instant
            // scroll carries the steps of the user's scroll along with it. A request that only aborts is for the main
            // thread's own scrolls.
            let mut shifts_running_animation = false;
            if let Some(index) = running_animation
                && !retargets_running_animation
            {
                let animation = &self.smooth_scroll_animations[index];
                let running_animation_is_requested = animation.operation_id >= 1u64 << 63;
                let running_animation_is_user_step = animation.is_user_scroll
                    && animation.animation.kind() != ScrollAnimationKind::Momentum
                    && !self
                        .scroll_snap_controller
                        .is_snap_scroll(scroll_request.stable_node_id, animation.operation_id);
                if smooth_scroll.is_some() {
                    self.cancel_smooth_scroll(scroll_request.stable_node_id);
                } else if has_instant_scroll && instant_scroll_is_for_script && running_animation_is_user_step {
                    shifts_running_animation = true;
                } else if has_instant_scroll || running_animation_is_requested {
                    self.cancel_smooth_scroll(scroll_request.stable_node_id);
                }
            } else if !retargets_running_animation && smooth_scroll.is_some() {
                self.scroll_snap_controller
                    .did_start_main_thread_scroll(scroll_request.stable_node_id);
            }
            let offset_before_instant_scroll = self
                .async_scroll_tree
                .scroll_offset_for_node(scroll_node_index, &self.scroll_state_snapshot)
                .unwrap();
            if let Some(jump_offset) = scroll_request.jump_offset {
                self.async_scroll_tree.set_scroll_offset(
                    scroll_node_index,
                    jump_offset,
                    self.scene.current_visual_context_tree(),
                    &mut self.scroll_state_snapshot,
                );
            }
            // A box new to this context took its offset from the frame's snapshot, which has the frame's scrolls in it.
            let offset_has_the_request_in_it = self
                .scroll_nodes_seeded_by_last_frame
                .contains(&scroll_request.stable_node_id);
            if let Some(relative_scroll) = &scroll_request.relative_scroll
                && !offset_has_the_request_in_it
            {
                if scroll_request.relative_scroll_moves_visual_viewport_first
                    && self.async_scroll_tree.scroll_node_is_viewport(scroll_node_index)
                {
                    self.scroll_visual_viewport_position_in_page(scroll_node_index, relative_scroll, &mut answers);
                } else {
                    let offset = self
                        .async_scroll_tree
                        .scroll_offset_for_node(scroll_node_index, &self.scroll_state_snapshot)
                        .unwrap();
                    self.async_scroll_tree.set_scroll_offset(
                        scroll_node_index,
                        relative_scroll.applied_to(offset),
                        self.scene.current_visual_context_tree(),
                        &mut self.scroll_state_snapshot,
                    );
                }
            }
            if let Some(axis) = axis_held_by_scrollbar
                && instant_scroll_is_for_script
            {
                let mut offset = self
                    .async_scroll_tree
                    .scroll_offset_for_node(scroll_node_index, &self.scroll_state_snapshot)
                    .unwrap();
                offset.set_primary_offset_for_orientation(
                    axis,
                    offset_before_instant_scroll.primary_offset_for_orientation(axis),
                );
                self.async_scroll_tree.set_scroll_offset(
                    scroll_node_index,
                    offset,
                    self.scene.current_visual_context_tree(),
                    &mut self.scroll_state_snapshot,
                );
            }
            let instant_scroll_delta = self
                .async_scroll_tree
                .scroll_offset_for_node(scroll_node_index, &self.scroll_state_snapshot)
                .unwrap()
                - offset_before_instant_scroll;
            if shifts_running_animation {
                let animation = &mut self.smooth_scroll_animations[running_animation.unwrap()];
                // A step that goes to an end of the scroll range still goes there.
                if !animation.heads_to_an_extent {
                    animation.animation.shift_by(instant_scroll_delta);
                    let destination_offset = animation.animation.destination_offset();
                    let clamped_destination_offset = self
                        .async_scroll_tree
                        .clamped_scroll_offset_for_node(scroll_node_index, destination_offset);
                    if clamped_destination_offset != destination_offset {
                        animation
                            .animation
                            .retarget(clamped_destination_offset, now - animation.started_at);
                    }
                }
            }
            if instant_scroll_is_for_script {
                self.elastic_overscroll_controller
                    .drop_stretch_moved_by(scroll_request.stable_node_id, instant_scroll_delta);
            }
            if smooth_scroll_is_for_script {
                self.elastic_overscroll_controller
                    .release_stretch_of(scroll_request.stable_node_id, now);
            }
            if let Some(smooth_scroll) = smooth_scroll {
                let mut destination_offset = smooth_scroll.destination_offset;
                if smooth_scroll.destination_is_relative {
                    let mut relative_scroll_start = self
                        .async_scroll_tree
                        .scroll_offset_for_node(scroll_node_index, &self.scroll_state_snapshot)
                        .unwrap();
                    if smooth_scroll.initiator == SmoothScrollInitiator::UserInput
                        && let Some(destination) = destination_of_running_user_scroll
                    {
                        relative_scroll_start = destination;
                    }
                    destination_offset = smooth_scroll.relative_scroll().applied_to(relative_scroll_start);
                }
                destination_offset = self
                    .async_scroll_tree
                    .clamped_scroll_offset_for_node(scroll_node_index, destination_offset);
                if retargets_running_animation {
                    self.retarget_user_scroll(running_animation.unwrap(), destination_offset, now);
                } else {
                    let start_offset = self
                        .async_scroll_tree
                        .scroll_offset_for_node(scroll_node_index, &self.scroll_state_snapshot)
                        .unwrap();
                    let animation = SmoothScrollAnimation::new(
                        start_offset,
                        destination_offset,
                        self.async_scroll_tree.device_pixels_per_css_pixel(),
                        smooth_scroll.animation_kind,
                    );
                    if animation.duration().is_zero() {
                        self.async_scroll_update_outbox
                            .complete_operation(smooth_scroll.operation_id);
                    } else {
                        self.smooth_scroll_animations.push(ActiveSmoothScrollAnimation {
                            stable_node_id: scroll_request.stable_node_id,
                            operation_id: smooth_scroll.operation_id,
                            animation,
                            started_at: now,
                            is_user_scroll: smooth_scroll.initiator == SmoothScrollInitiator::UserInput,
                            heads_to_an_extent: false,
                        });
                    }
                }
            }
            answer_with_the_offset_of_the_box(self, &mut answers, &mut moved_the_box);
            note_request_applied(self, moved_the_box);
        }
        if answers.is_empty() {
            self.request_rendering_update();
            return None;
        }
        self.store_pending_async_scroll_offsets(&answers, None);
        if self.has_async_scrolling_state {
            self.rebuild_wheel_hit_test_targets();
        }
        self.request_rendering_update();
        Some(PendingFrame::repainting_changes(self.async_scrolling_viewport_rect))
    }

    // https://drafts.csswg.org/cssom-view/#viewport-perform-a-scroll
    // The visual viewport takes as much of the scroll as it can before the layout viewport takes the rest.
    fn scroll_visual_viewport_position_in_page(
        &mut self,
        viewport_scroll_node_index: SpatialNodeIndex,
        relative_scroll: &RequestedRelativeScroll,
        answers: &mut Vec<AsyncScrollOffset>,
    ) {
        let Some(layout_viewport_offset) = self
            .async_scroll_tree
            .scroll_offset_for_node(viewport_scroll_node_index, &self.scroll_state_snapshot)
        else {
            return;
        };
        let Some(document_id) = self.async_scroll_tree.document_id() else {
            return;
        };
        let mut transform = self.scene.visual_viewport_transform();
        let scale = transform.matrix.elements[0][0];
        let old_visual_viewport_offset = visual_viewport_scroll_offset(&transform);
        let page_position = layout_viewport_offset + old_visual_viewport_offset;
        let delta = relative_scroll.applied_to(page_position) - page_position;
        let share_of_viewport_the_visual_viewport_moves_over = if scale <= 1.0 { 0.0 } else { (scale - 1.0) / scale };
        let maximum_visual_viewport_offset = FloatPoint {
            x: self.async_scrolling_viewport_rect.width as f32 * share_of_viewport_the_visual_viewport_moves_over,
            y: self.async_scrolling_viewport_rect.height as f32 * share_of_viewport_the_visual_viewport_moves_over,
        };
        let new_visual_viewport_offset = FloatPoint {
            x: clamp(
                old_visual_viewport_offset.x + delta.x,
                0.0,
                maximum_visual_viewport_offset.x,
            ),
            y: clamp(
                old_visual_viewport_offset.y + delta.y,
                0.0,
                maximum_visual_viewport_offset.y,
            ),
        };
        let visual_delta = new_visual_viewport_offset - old_visual_viewport_offset;
        self.async_scroll_tree.set_scroll_offset(
            viewport_scroll_node_index,
            layout_viewport_offset + (delta - visual_delta),
            self.scene.current_visual_context_tree(),
            &mut self.scroll_state_snapshot,
        );
        if visual_delta.is_zero() {
            return;
        }
        transform.matrix.elements[0][3] = -new_visual_viewport_offset.x * scale;
        transform.matrix.elements[1][3] = -new_visual_viewport_offset.y * scale;
        self.scene.set_async_visual_viewport_transform(transform);
        self.visual_viewport_document_id = Some(document_id);
        answers.push(AsyncScrollOffset {
            stable_node_id: AsyncScrollNodeStableID {
                node_id: document_id,
                kind: AsyncScrollNodeKind::VisualViewport,
                pseudo_element_type: 0,
            },
            compositor_scroll_offset: new_visual_viewport_offset,
            unadopted_scroll_delta: visual_delta,
            last_relative_scroll_delta: Default::default(),
            visual_viewport_scale: Some(scale),
            applied_scroll_request_id: 0,
            user_input_scroll_count: 0,
            context_user_input_scroll_count: 0,
        });
    }

    // Returns whether the request moved the visual viewport.
    fn apply_visual_viewport_scroll_request(
        &mut self,
        scroll_request: &ScrollRequest,
        answers: &mut Vec<AsyncScrollOffset>,
    ) -> bool {
        // The main thread animates the visual viewport itself, one jump at a time.
        if let Some(scroll) = &scroll_request.smooth_scroll {
            self.async_scroll_update_outbox.complete_operation(scroll.operation_id);
        }
        if self.scene.visual_context_tree().is_none()
            || Some(scroll_request.stable_node_id.node_id) != self.async_scroll_tree.document_id()
        {
            return false;
        }
        let mut transform = self.scene.visual_viewport_transform();
        let old_scroll_offset = visual_viewport_scroll_offset(&transform);
        if let Some(jump_offset) = scroll_request.jump_offset {
            let scale = scroll_request
                .visual_viewport_scale
                .unwrap_or(transform.matrix.elements[0][0]);
            transform.matrix.elements[0][0] = scale;
            transform.matrix.elements[1][1] = scale;
            transform.matrix.elements[0][3] = -jump_offset.x * scale;
            transform.matrix.elements[1][3] = -jump_offset.y * scale;
            clamp_visual_viewport_transform_to_viewport(&mut transform, self.async_scrolling_viewport_rect);
            self.scene.set_async_visual_viewport_transform(transform);
            self.visual_viewport_document_id = Some(scroll_request.stable_node_id.node_id);
            self.rebuild_wheel_hit_test_targets();
        }
        let new_scroll_offset = visual_viewport_scroll_offset(&transform);
        answers.push(AsyncScrollOffset {
            stable_node_id: scroll_request.stable_node_id,
            compositor_scroll_offset: new_scroll_offset,
            unadopted_scroll_delta: new_scroll_offset - old_scroll_offset,
            last_relative_scroll_delta: Default::default(),
            visual_viewport_scale: Some(transform.matrix.elements[0][0]),
            applied_scroll_request_id: 0,
            user_input_scroll_count: 0,
            context_user_input_scroll_count: 0,
        });
        new_scroll_offset != old_scroll_offset
    }

    pub(super) fn in_flight_wheel_destination(&self, stable_node_id: AsyncScrollNodeStableID) -> Option<FloatPoint> {
        for animation in &self.smooth_scroll_animations {
            if animation.stable_node_id == stable_node_id && animation.animation.kind() == ScrollAnimationKind::Wheel {
                return Some(animation.animation.destination_offset());
            }
        }
        None
    }

    fn smooth_wheel_scroll_by(
        &mut self,
        scroll_target: Option<&WheelScrollTarget>,
        viewport_rect: IntRect,
        now: MonotonicTime,
    ) -> AsyncScrollResult {
        let Some(scroll_target) = scroll_target else {
            let operation_id = self.async_scroll_update_outbox.allocate_operation_id();
            self.async_scroll_update_outbox.complete_operation(operation_id);
            return AsyncScrollResult {
                enqueue_result: AsyncScrollEnqueueResult {
                    accepted: true,
                    operation_id: Some(operation_id),
                    ..Default::default()
                },
                frame_to_present: None,
            };
        };
        for index in 0..self.smooth_scroll_animations.len() {
            let active_animation = &self.smooth_scroll_animations[index];
            if active_animation.stable_node_id != scroll_target.stable_node_id {
                continue;
            }
            if active_animation.animation.kind() == ScrollAnimationKind::Wheel {
                self.retarget_user_scroll(index, scroll_target.destination_offset, now);
                let operation_id = self.smooth_scroll_animations[index].operation_id;
                let kind = self.smooth_scroll_animations[index].animation.kind();
                self.report_started_user_scroll(
                    operation_id,
                    scroll_target.stable_node_id,
                    scroll_target.start_offset,
                    scroll_target.destination_offset,
                    kind,
                    false,
                );
                self.async_scrolling_viewport_rect = viewport_rect;
                return AsyncScrollResult {
                    enqueue_result: AsyncScrollEnqueueResult {
                        accepted: true,
                        operation_id: Some(operation_id),
                        ..Default::default()
                    },
                    frame_to_present: Some(PendingFrame::repainting_changes(viewport_rect)),
                };
            }
            self.async_scroll_update_outbox
                .note_operation_taken_over_by_user_input(active_animation.operation_id);
            self.cancel_smooth_scroll(scroll_target.stable_node_id);
            break;
        }
        let operation_id = self.async_scroll_update_outbox.allocate_operation_id();
        self.smooth_scroll_animations.push(ActiveSmoothScrollAnimation {
            stable_node_id: scroll_target.stable_node_id,
            operation_id,
            animation: SmoothScrollAnimation::new(
                scroll_target.current_offset,
                scroll_target.destination_offset,
                self.async_scroll_tree.device_pixels_per_css_pixel(),
                ScrollAnimationKind::Wheel,
            ),
            started_at: now,
            is_user_scroll: true,
            heads_to_an_extent: false,
        });
        self.report_started_user_scroll(
            operation_id,
            scroll_target.stable_node_id,
            scroll_target.start_offset,
            scroll_target.destination_offset,
            ScrollAnimationKind::Wheel,
            false,
        );
        self.async_scrolling_viewport_rect = viewport_rect;
        AsyncScrollResult {
            enqueue_result: AsyncScrollEnqueueResult {
                accepted: true,
                operation_id: Some(operation_id),
                ..Default::default()
            },
            frame_to_present: Some(PendingFrame::repainting_changes(viewport_rect)),
        }
    }

    pub(super) fn retarget_user_scroll(&mut self, index: usize, destination_offset: FloatPoint, now: MonotonicTime) {
        let running_animation = &mut self.smooth_scroll_animations[index];
        self.scroll_snap_controller
            .did_retarget_scroll(running_animation.stable_node_id, running_animation.operation_id);
        running_animation
            .animation
            .retarget(destination_offset, now - running_animation.started_at);
    }

    pub fn cancel_smooth_scroll(&mut self, stable_node_id: AsyncScrollNodeStableID) {
        self.retire_smooth_scroll_animation(stable_node_id);
        self.scroll_snap_controller.did_start_main_thread_scroll(stable_node_id);
    }

    fn retire_smooth_scroll_animation(&mut self, stable_node_id: AsyncScrollNodeStableID) {
        for index in 0..self.smooth_scroll_animations.len() {
            let animation = &self.smooth_scroll_animations[index];
            if animation.stable_node_id != stable_node_id {
                continue;
            }
            if self
                .scroll_snap_controller
                .is_snap_scroll(stable_node_id, animation.operation_id)
            {
                let mut scroll_offset = None;
                if let Some(scroll_node_index) = self.async_scroll_tree.scroll_node_index_for_stable_id(stable_node_id)
                {
                    scroll_offset = self
                        .async_scroll_tree
                        .css_scroll_offset_for_node(scroll_node_index, &self.scroll_state_snapshot);
                }
                self.scroll_snap_controller
                    .did_end_snap_scroll(stable_node_id, animation.operation_id, scroll_offset);
            }
            self.async_scroll_update_outbox
                .complete_operation(animation.operation_id);
            self.smooth_scroll_animations.remove(index);
            self.request_rendering_update();
            return;
        }
    }

    pub fn advance_smooth_scroll_animations(&mut self, now: MonotonicTime) -> Option<IntRect> {
        let mut scroll_offsets = Vec::new();
        let mut changed_scroll_offset = false;
        let mut completed_operation = false;
        let mut index = 0;
        while index < self.smooth_scroll_animations.len() {
            let active_animation = &self.smooth_scroll_animations[index];
            let scroll_node_index = self
                .async_scroll_tree
                .scroll_node_index_for_stable_id(active_animation.stable_node_id);
            let Some(scroll_node_index) = scroll_node_index else {
                self.async_scroll_update_outbox
                    .complete_operation(active_animation.operation_id);
                completed_operation = true;
                self.smooth_scroll_animations.remove(index);
                continue;
            };
            let old_offset = self
                .async_scroll_tree
                .scroll_offset_for_node(scroll_node_index, &self.scroll_state_snapshot);
            let Some(old_offset) = old_offset else {
                self.async_scroll_update_outbox
                    .complete_operation(active_animation.operation_id);
                completed_operation = true;
                self.smooth_scroll_animations.remove(index);
                continue;
            };
            let sample = active_animation.animation.sample(now - active_animation.started_at);
            let new_offset = self.async_scroll_tree.set_scroll_offset(
                scroll_node_index,
                sample.offset,
                self.scene.current_visual_context_tree(),
                &mut self.scroll_state_snapshot,
            );
            assert!(new_offset.is_some());
            let new_offset = new_offset.unwrap();
            let scroll_delta = new_offset - old_offset;
            if !scroll_delta.is_zero() {
                scroll_offsets.push(AsyncScrollOffset {
                    stable_node_id: active_animation.stable_node_id,
                    compositor_scroll_offset: new_offset,
                    unadopted_scroll_delta: scroll_delta,
                    last_relative_scroll_delta: Default::default(),
                    visual_viewport_scale: None,
                    applied_scroll_request_id: 0,
                    user_input_scroll_count: 0,
                    context_user_input_scroll_count: 0,
                });
                changed_scroll_offset = true;
                if self.async_scroll_tree.scroll_node_is_viewport(scroll_node_index) {
                    self.async_scrolling_viewport_rect.x = new_offset.x as i32;
                    self.async_scrolling_viewport_rect.y = new_offset.y as i32;
                }
            }
            if sample.complete {
                self.scroll_snap_controller.did_end_snap_scroll(
                    active_animation.stable_node_id,
                    active_animation.operation_id,
                    Some(self.async_scroll_tree.css_pixels_from_device_offset(new_offset)),
                );
                self.async_scroll_update_outbox
                    .complete_operation(active_animation.operation_id);
                completed_operation = true;
                self.smooth_scroll_animations.remove(index);
            } else {
                index += 1;
            }
        }
        if !scroll_offsets.is_empty() {
            self.rebuild_wheel_hit_test_targets();
            self.store_pending_async_scroll_offsets(&scroll_offsets, None);
        }
        if changed_scroll_offset || completed_operation {
            self.request_rendering_update();
        }
        if changed_scroll_offset {
            return Some(self.async_scrolling_viewport_rect);
        }
        None
    }
}
