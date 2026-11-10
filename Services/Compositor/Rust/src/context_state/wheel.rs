/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Wheel event routing for a compositor context.

use super::*;

impl WheelStep {
    pub fn for_wheel_event(
        event: &MouseEvent,
        gesture_id: Option<u64>,
        operation_tracking: AsyncScrollOperationTracking,
    ) -> Self {
        let mut delta_in_css_pixels = FloatPoint {
            x: event.wheel_delta_x as f32,
            y: event.wheel_delta_y as f32,
        };
        if !(event.modifiers & KeyModifier::Mod_Shift).is_empty() {
            delta_in_css_pixels = FloatPoint {
                x: delta_in_css_pixels.y,
                y: delta_in_css_pixels.x,
            };
        }
        Self {
            position: ContextState::wheel_event_position(event),
            delta_in_css_pixels,
            wheel_delta_precision: event.wheel_delta_precision,
            scroll_gesture_phase: event.scroll_gesture_phase,
            gesture_id,
            operation_tracking,
        }
    }
}

impl ContextState {
    // The UI's wheel deltas are in CSS pixels; this scales them for the scroll tree, which works in device pixels.
    pub fn handle_wheel_event(
        &mut self,
        event: &MouseEvent,
        is_first_wheel_event_with_delta: bool,
        listener_gate: &WheelEventListenerGate,
        now: MonotonicTime,
    ) -> ContextUpdateResult {
        assert_eq!(event.r#type, MouseEventType::MouseWheel);
        if !self.presents_to_client() {
            return ContextUpdateResult::declined(InputRoutingDecision::ContextNotPresenting);
        }
        self.scroll_fling_controller.note_wheel_event(event, now);
        self.scroll_for_wheel_event(
            event,
            is_first_wheel_event_with_delta,
            listener_gate,
            AsyncScrollOperationTracking::No,
            now,
        )
    }

    // Routes the event like one from the UI, also in a context that is nested in another one.
    pub fn scroll_for_wheel_event_from_main_thread(
        &mut self,
        event: &MouseEvent,
        is_first_wheel_event_with_delta: bool,
        listener_gate: &WheelEventListenerGate,
        operation_tracking: AsyncScrollOperationTracking,
        now: MonotonicTime,
    ) -> ContextUpdateResult {
        assert_eq!(event.r#type, MouseEventType::MouseWheel);
        self.scroll_for_wheel_event(
            event,
            is_first_wheel_event_with_delta,
            listener_gate,
            operation_tracking,
            now,
        )
    }

    fn wheel_event_position(event: &MouseEvent) -> FloatPoint {
        FloatPoint {
            x: event.position.x as f32,
            y: event.position.y as f32,
        }
    }

    fn wheel_event_delta_in_device_pixels(&self, event: &MouseEvent) -> FloatPoint {
        let mut wheel_delta_x = event.wheel_delta_x;
        let mut wheel_delta_y = event.wheel_delta_y;
        if !(event.modifiers & KeyModifier::Mod_Shift).is_empty() {
            std::mem::swap(&mut wheel_delta_x, &mut wheel_delta_y);
        }
        FloatPoint {
            x: wheel_delta_x as f32,
            y: wheel_delta_y as f32,
        }
        .scaled(self.async_scroll_tree.device_pixels_per_css_pixel() as f32)
    }

    pub fn wheel_event_listener_gate(&self, gesture_id: Option<u64>) -> WheelEventListenerGate {
        WheelEventListenerGate {
            page_let_the_gesture_through: self.wheel_gesture_routing.page_let_gesture_through(gesture_id),
            reason_to_hold_latched_steps_back: self.wheel_gesture_routing.reason_to_hold_latched_steps_back(gesture_id),
        }
    }

    // A wheel event of the UI that this context did not scroll by is left to its page, which then answers whether it let
    // the event through.
    pub fn note_wheel_event_left_to_the_page(&mut self, event: &MouseEvent) {
        self.wheel_gesture_routing.note_delta_left_to_the_page(
            WheelStep::for_wheel_event(event, None, AsyncScrollOperationTracking::No).delta_in_css_pixels,
        );
    }

    fn note_wheel_event(&mut self, event: &MouseEvent, now: MonotonicTime) -> NotedWheelEvent {
        assert_eq!(event.r#type, MouseEventType::MouseWheel);
        // The end of the gesture before this one may never have arrived.
        let noted = self.wheel_gesture_routing.note_wheel_event(
            event,
            ContextState::wheel_event_position(event),
            &self.async_scroll_tree,
            now,
        );
        if noted.starts_gesture {
            self.elastic_overscroll_controller.end_gesture(now);
        }
        NotedWheelEvent {
            wheel_gesture_mark: WheelGestureMark {
                gesture_id: self.wheel_gesture_routing.gesture_id().unwrap(),
                lasts_until_fingers_lift: self.wheel_gesture_routing.gesture_lasts_until_fingers_lift(),
                step_number: noted.step_number,
            },
            is_first_wheel_event_with_delta: noted.is_first_wheel_event_with_delta,
        }
    }

    // Takes note of a wheel event from the UI as it arrives, ahead of anything that may turn it away or leave it to
    // the page.
    pub fn note_wheel_event_from_ui(&mut self, event: &MouseEvent, now: MonotonicTime) -> NotedWheelEvent {
        let phase = event.scroll_gesture_phase;
        let noted = self.note_wheel_event(event, now);
        if phase == ScrollGesturePhase::Momentum {
            self.last_reported_scroll_momentum_step = Some(event.clone());
            self.last_reported_scroll_momentum_step
                .as_mut()
                .unwrap()
                .wheel_gesture_mark = Some(noted.wheel_gesture_mark.clone());
        } else {
            self.last_reported_scroll_momentum_step = None;
        }
        // A stretch is released with the velocity of the steps as they arrived from the UI, not as they were scrolled.
        // Fling steps are not timed here, which leaves them out of the velocity; no platform both flings and stretches.
        if self.wheel_step_can_stretch_elastically(event.wheel_delta_precision, phase) {
            let delta = self
                .wheel_event_delta_in_device_pixels(event)
                .scaled(1.0 / self.scene.visual_viewport_magnification());
            self.elastic_overscroll_controller.note_step_from_ui(delta, now);
        }
        noted
    }

    // Takes note of a wheel event that the main thread has and the UI did not send here, as the UI's events are noted.
    // Such an event adds nothing to the velocity of a flick or of a stretch release.
    pub fn note_wheel_event_from_main_thread(&mut self, event: &MouseEvent, now: MonotonicTime) -> NotedWheelEvent {
        self.note_wheel_event(event, now)
    }

    // Starts a fling if the gesture that the event ends was a flick. Use only on platforms that send no momentum
    // scroll events.
    pub fn start_scroll_fling_if_flicked(&mut self, gesture_end_event: &MouseEvent, now: MonotonicTime) -> bool {
        self.scroll_fling_controller.start_if_flicked(gesture_end_event, now)
    }

    // Ends the fling before a wheel event from a new gesture or other input.
    pub fn interrupt_scroll_fling(&mut self, event: &MouseEvent, now: MonotonicTime) -> Option<MouseEvent> {
        self.scroll_fling_controller.interrupt(event, now)
    }

    fn interrupt_reported_scroll_momentum(&mut self) -> Option<MouseEvent> {
        self.last_reported_scroll_momentum_step.as_ref()?;
        // WebContent holds the gesture open while momentum events arrive. The end of the gesture releases it.
        let mut gesture_end = self.last_reported_scroll_momentum_step.take().unwrap();
        gesture_end.wheel_delta_x = 0.0;
        gesture_end.wheel_delta_y = 0.0;
        gesture_end.wheel_tick_120ths_x = 0.0;
        gesture_end.wheel_tick_120ths_y = 0.0;
        gesture_end.scroll_gesture_phase = ScrollGesturePhase::Ended;
        gesture_end.async_scroll_performed_default_action = false;
        // The IDs of UI input events start at 1. Thus, ID 0 does not match a UI event.
        gesture_end.id = 0;
        self.reported_scroll_momentum_was_interrupted = true;
        Some(gesture_end)
    }

    // Ends the fling, or the gesture whose momentum the platform is reporting, if it scrolls the box, so that the rest of
    // its momentum scrolls nothing. Returns the end of the gesture to send on to WebContent.
    pub fn end_momentum_scrolling(&mut self, stable_node_id: AsyncScrollNodeStableID) -> Option<MouseEvent> {
        if self.wheel_gesture_routing.latched_scroller() != Some(stable_node_id) {
            return None;
        }
        if self.has_active_scroll_fling() {
            return self.end_scroll_fling();
        }
        self.interrupt_reported_scroll_momentum()
    }

    pub fn clear_per_test_input_state(&mut self) {
        // A gesture held by fingers never ends by itself, so one a test left unended would take in the next test's steps.
        self.wheel_gesture_routing.forget_gesture();
        self.pointer_in_nested_contexts = Default::default();
        self.last_reported_scroll_momentum_step = None;
        self.reported_scroll_momentum_was_interrupted = false;
    }

    // Whether the wheel event from the UI is momentum, or the end, of a gesture whose momentum was interrupted.
    pub fn absorbs_wheel_event_of_interrupted_momentum(&mut self, event: &MouseEvent) -> bool {
        if !self.reported_scroll_momentum_was_interrupted {
            return false;
        }
        if matches!(
            event.scroll_gesture_phase,
            ScrollGesturePhase::Momentum | ScrollGesturePhase::Ended
        ) {
            return true;
        }
        self.reported_scroll_momentum_was_interrupted = false;
        false
    }

    // The wheel event of the next step of the fling, which is scrolled by like a wheel event from the UI.
    pub fn take_scroll_fling_step(&mut self, frame_time: MonotonicTime) -> Option<MouseEvent> {
        self.scroll_fling_controller.take_step(frame_time)
    }
    pub fn end_scroll_fling(&mut self) -> Option<MouseEvent> {
        self.scroll_fling_controller.end()
    }

    pub fn scroll_for_fling_step(
        &mut self,
        event: &MouseEvent,
        listener_gate: &WheelEventListenerGate,
        now: MonotonicTime,
    ) -> ContextUpdateResult {
        if !self.presents_to_client() {
            return ContextUpdateResult::declined(InputRoutingDecision::ContextNotPresenting);
        }
        self.scroll_for_wheel_event(event, false, listener_gate, AsyncScrollOperationTracking::No, now)
    }

    pub fn end_scroll_fling_on_next_step(&mut self) {
        self.scroll_fling_controller.end_on_next_step();
    }

    fn scroll_for_wheel_event(
        &mut self,
        event: &MouseEvent,
        is_first_wheel_event_with_delta: bool,
        listener_gate: &WheelEventListenerGate,
        operation_tracking: AsyncScrollOperationTracking,
        now: MonotonicTime,
    ) -> ContextUpdateResult {
        let step = WheelStep::for_wheel_event(event, self.wheel_gesture_routing.gesture_id(), operation_tracking);
        // The end of a gesture lets go of the scroller it stretched even if the context has stopped taking wheel events.
        if step.scroll_gesture_phase == ScrollGesturePhase::Ended {
            self.elastic_overscroll_controller.end_gesture(now);
        }
        let latch = self.resolve_wheel_scroll_latch(step.scroll_gesture_phase, step.gesture_id);
        if let Some(context_id) = latch.nested_context_id {
            return ContextUpdateResult {
                nested_context_wheel_step: Some(NestedContextWheelStep { context_id, hit: None }),
                ..Default::default()
            };
        }
        if latch.scroll_node_index.is_some() {
            // A blocking listener added during the gesture holds its later steps back until the page lets one through.
            if let Some(reason) = listener_gate.reason_to_hold_latched_steps_back {
                return ContextUpdateResult::declined(reason);
            }
            let admission = if listener_gate.page_let_the_gesture_through {
                WheelStepAdmission::LetThroughByThePage
            } else {
                WheelStepAdmission::ByThisPage
            };
            return self.scroll_for_wheel_step_in_this_context(
                &step,
                latch.scroll_node_index,
                NestedContextRegions::LeaveToMainThread,
                admission,
                &[],
                now,
            );
        }
        if listener_gate.page_let_the_gesture_through {
            return self.scroll_for_wheel_step_in_this_context(
                &step,
                None,
                NestedContextRegions::Ignore,
                WheelStepAdmission::LetThroughByThePage,
                &[],
                now,
            );
        }
        // The first step of a gesture over the content of a nested context goes to that context. A gesture whose first step
        // went elsewhere is not handed to a nested context that comes under the pointer later.
        let mut nested_context_regions = NestedContextRegions::LeaveToMainThread;
        if is_first_wheel_event_with_delta && self.may_hand_first_wheel_steps_to_nested_contexts() {
            nested_context_regions = NestedContextRegions::HandToNestedContext;
            if let Some(hit) = self.nested_context_for_wheel_at(step.position) {
                return ContextUpdateResult {
                    nested_context_wheel_step: Some(NestedContextWheelStep {
                        context_id: hit.child_context_id,
                        hit: Some(hit),
                    }),
                    ..Default::default()
                };
            }
        }
        self.scroll_for_wheel_step_in_this_context(
            &step,
            None,
            nested_context_regions,
            WheelStepAdmission::ByThisPage,
            &[],
            now,
        )
    }

    pub fn scroll_for_wheel_event_passed_on_by_nested_context(
        &mut self,
        event: &MouseEvent,
        operation_tracking: AsyncScrollOperationTracking,
        now: MonotonicTime,
    ) -> ContextUpdateResult {
        let step = WheelStep::for_wheel_event(event, self.wheel_gesture_routing.gesture_id(), operation_tracking);
        self.scroll_for_wheel_step_in_this_context(
            &step,
            None,
            NestedContextRegions::Ignore,
            WheelStepAdmission::PassedOnByNestedContext,
            &[],
            now,
        )
    }

    // The first step of a gesture that the page let through, routed from the scrollers the page found around its target,
    // innermost first, if this context knows one, and by a hit test otherwise.
    pub fn scroll_for_first_wheel_step_the_page_let_through(
        &mut self,
        step: &WheelStep,
        scroll_chain: &[AsyncScrollNodeStableID],
        now: MonotonicTime,
    ) -> ContextUpdateResult {
        self.scroll_for_wheel_step_in_this_context(
            step,
            None,
            NestedContextRegions::Ignore,
            WheelStepAdmission::LetThroughByThePage,
            scroll_chain,
            now,
        )
    }

    // A step of a gesture that the page let through and that is latched to a scroller of this context. Empty once the
    // scroller is gone.
    pub fn scroll_for_latched_wheel_step_the_page_let_through(
        &mut self,
        step: &WheelStep,
        now: MonotonicTime,
    ) -> Option<ContextUpdateResult> {
        let latch = self.resolve_wheel_scroll_latch(step.scroll_gesture_phase, step.gesture_id);
        latch.scroll_node_index?;
        Some(self.scroll_for_wheel_step_in_this_context(
            step,
            latch.scroll_node_index,
            NestedContextRegions::Ignore,
            WheelStepAdmission::LetThroughByThePage,
            &[],
            now,
        ))
    }

    // A step of a gesture that this context has moved past scrolls once along its route, latching and stretching
    // nothing.
    pub fn scroll_for_overtaken_wheel_step(
        &mut self,
        step: &WheelStep,
        scroll_chain: &[AsyncScrollNodeStableID],
        now: MonotonicTime,
    ) -> ContextUpdateResult {
        if !self.has_async_scrolling_state {
            return ContextUpdateResult::declined(InputRoutingDecision::NoAsyncScrollingState);
        }
        let delta = step
            .delta_in_css_pixels
            .scaled(self.async_scroll_tree.device_pixels_per_css_pixel() as f32);
        if delta.is_zero() {
            return ContextUpdateResult::declined(InputRoutingDecision::StepOvertaken);
        }
        let scroll_target = self.route_first_wheel_step(
            step.position,
            delta,
            step.wheel_delta_precision,
            WheelStepDispatch::DispatchedAndNotCanceled,
            NestedContextRegions::Ignore,
            scroll_chain,
        );
        let Some(scroll_node_index) = scroll_target.scroll_node_index else {
            return ContextUpdateResult::declined(InputRoutingDecision::StepOvertaken);
        };
        self.scroll_snap_controller
            .note_gesture_phase(step.scroll_gesture_phase);
        let node_scroll = self.scroll_node_by_wheel_delta(
            scroll_node_index,
            delta,
            step.wheel_delta_precision,
            step.scroll_gesture_phase,
            step.operation_tracking,
            self.async_scrolling_viewport_rect,
            ScrollChaining::ToScrollableAncestors,
            now,
        );
        let outcome = node_scroll.outcome;
        ContextUpdateResult {
            accepted: true,
            frame_to_present: outcome.viewport_rect_to_present.map(PendingFrame::repainting_changes),
            should_request_rendering_update: outcome.viewport_rect_to_present.is_some(),
            routing_decision: Some(InputRoutingDecision::StepOvertaken),
            tracked_scroll_operation: self.tracked_scroll_operation(step.operation_tracking, outcome.operation_id),
            ..Default::default()
        }
    }

    fn tracked_scroll_operation(
        &self,
        operation_tracking: AsyncScrollOperationTracking,
        operation_id: Option<AsyncScrollOperationID>,
    ) -> Option<TrackedScrollOperation> {
        if operation_tracking == AsyncScrollOperationTracking::No || operation_id.is_none() {
            return None;
        }
        Some(TrackedScrollOperation {
            context_id: self.context_id,
            operation_id: operation_id.unwrap(),
        })
    }

    // A step of the gesture latched to the scroller, or the first step, which latches the gesture to the scroller it is
    // routed to.
    fn scroll_for_wheel_step_in_this_context(
        &mut self,
        step: &WheelStep,
        mut latched_scroll_node_index: Option<SpatialNodeIndex>,
        nested_context_regions: NestedContextRegions,
        admission: WheelStepAdmission,
        scroll_chain: &[AsyncScrollNodeStableID],
        now: MonotonicTime,
    ) -> ContextUpdateResult {
        let delta = step
            .delta_in_css_pixels
            .scaled(self.async_scroll_tree.device_pixels_per_css_pixel() as f32);
        let is_first_step_of_gesture = latched_scroll_node_index.is_none();
        let found_nothing_to_scroll = |decision| {
            let there_is_nothing_to_scroll = matches!(
                decision,
                InputRoutingDecision::NoScrollTarget
                    | InputRoutingDecision::NoScrollNode
                    | InputRoutingDecision::GestureEndWithNothingToSnap
            );
            if admission == WheelStepAdmission::LetThroughByThePage && there_is_nothing_to_scroll {
                return ContextUpdateResult {
                    accepted: true,
                    routing_decision: Some(decision),
                    ..Default::default()
                };
            }
            ContextUpdateResult::declined(decision)
        };
        if is_first_step_of_gesture {
            let routing_admission = self.wheel_gesture_routing.admission();
            // A page without scroll nodes still has a visual viewport to pan once it is zoomed in.
            let can_pan_zoomed_visual_viewport = routing_admission == WheelRoutingAdmission::NoScrollNode
                && self.scene.visual_viewport_scale().unwrap_or(1.0) > 1.0;
            // Only the listeners of the page that sees the step may hold it back.
            let is_admitted = routing_admission == WheelRoutingAdmission::Accepted
                || (admission != WheelStepAdmission::ByThisPage
                    && matches!(
                        routing_admission,
                        WheelRoutingAdmission::BlockingWheelEventListeners
                            | WheelRoutingAdmission::StaleWheelEventListeners
                    ));
            if !is_admitted && !can_pan_zoomed_visual_viewport {
                let decision = input_routing_decision_for_wheel_step_not_admitted_by(routing_admission);
                if routing_admission == WheelRoutingAdmission::NoScrollNode {
                    return found_nothing_to_scroll(decision);
                }
                return ContextUpdateResult::declined(decision);
            }
        }
        self.scroll_snap_controller
            .note_gesture_phase(step.scroll_gesture_phase);
        if step.scroll_gesture_phase == ScrollGesturePhase::Ended && delta.is_zero() {
            let operation_id = self.snap_at_gesture_end(now);
            if operation_id.is_none() {
                return found_nothing_to_scroll(InputRoutingDecision::GestureEndWithNothingToSnap);
            }
            return ContextUpdateResult {
                accepted: true,
                frame_to_present: Some(PendingFrame::repainting_changes(self.async_scrolling_viewport_rect)),
                should_request_rendering_update: true,
                routing_decision: Some(InputRoutingDecision::SnappedAtGestureEnd),
                tracked_scroll_operation: self.tracked_scroll_operation(step.operation_tracking, operation_id),
                ..Default::default()
            };
        }
        // Whether a gesture waits for the page is decided by its first step; the steps of a latched gesture are not held
        // back by regions the cursor moves over later.
        let wheel_step_dispatch = if admission == WheelStepAdmission::ByThisPage {
            WheelStepDispatch::NotYetDispatched
        } else {
            WheelStepDispatch::DispatchedAndNotCanceled
        };
        if is_first_step_of_gesture && wheel_step_dispatch == WheelStepDispatch::NotYetDispatched {
            let initial_scroll_target = self.async_scroll_tree.hit_test_scroll_node_for_wheel(
                self.scene.visual_context_tree_for_compositing(),
                step.position,
                delta,
                &|_| None,
                wheel_step_dispatch,
                nested_context_regions,
            );
            if initial_scroll_target.is_blocked_by_a_region() {
                return ContextUpdateResult::declined(input_routing_decision_for_first_wheel_step(
                    initial_scroll_target.outcome,
                ));
            }
        }
        if delta.is_zero() {
            return ContextUpdateResult {
                accepted: true,
                routing_decision: Some(InputRoutingDecision::StepWithoutDelta),
                routed_scroller: self.wheel_gesture_routing.latched_scroller(),
                ..Default::default()
            };
        }
        // A gesture is latched before its first step pans the visual viewport, so that the scroller it belongs to is known
        // for as long as the gesture only pans.
        let mut routing_decision = InputRoutingDecision::ContinuedLatch;
        if is_first_step_of_gesture {
            let latch_attempt = self.route_and_latch_wheel_gesture(
                step.position,
                delta.scaled(1.0 / self.scene.visual_viewport_magnification()),
                step.wheel_delta_precision,
                step.scroll_gesture_phase,
                step.gesture_id,
                wheel_step_dispatch,
                nested_context_regions,
                scroll_chain,
            );
            latched_scroll_node_index = latch_attempt.latched_scroll_node_index;
            routing_decision = latch_attempt.decision;
        }
        // A zoomed page still pans its visual viewport by a gesture that no scroller takes.
        let Some(latched_scroll_node_index) = latched_scroll_node_index else {
            if self.pan_visual_viewport_by(delta).is_zero() {
                return found_nothing_to_scroll(routing_decision);
            }
            return ContextUpdateResult {
                accepted: true,
                frame_to_present: Some(PendingFrame::repainting_changes(self.async_scrolling_viewport_rect)),
                should_request_rendering_update: true,
                routing_decision: Some(InputRoutingDecision::PannedVisualViewport),
                ..Default::default()
            };
        };
        let scroll_chaining = if is_first_step_of_gesture {
            ScrollChaining::ToScrollableAncestors
        } else {
            ScrollChaining::None
        };
        let outcome = self.perform_wheel_scroll_of_node(
            latched_scroll_node_index,
            delta,
            step.wheel_delta_precision,
            step.scroll_gesture_phase,
            step.operation_tracking,
            self.async_scrolling_viewport_rect,
            scroll_chaining,
            VisualViewportPanning::BeforeScrolling,
            now,
        );
        ContextUpdateResult {
            accepted: true,
            frame_to_present: outcome.viewport_rect_to_present.map(PendingFrame::repainting_changes),
            should_request_rendering_update: outcome.viewport_rect_to_present.is_some()
                && !outcome.only_elastic_overscroll_changed,
            routing_decision: Some(routing_decision),
            routed_scroller: self.wheel_gesture_routing.latched_scroller(),
            tracked_scroll_operation: self.tracked_scroll_operation(step.operation_tracking, outcome.operation_id),
            ..Default::default()
        }
    }

    pub fn may_hand_first_wheel_steps_to_nested_contexts(&self) -> bool {
        // A zoomed page pans its visual viewport before anything scrolls. This document's wheel event listeners never see
        // the wheel events over a document nested in it, whatever state they are in.
        self.has_async_scrolling_state && self.scene.visual_viewport_scale().unwrap_or(1.0) == 1.0
    }

    // Whether this context's page may cancel the ctrl+wheel event of a pinch at the position, or has yet to tell.
    pub fn page_may_cancel_pinch_at(&mut self, position: FloatPoint) -> bool {
        if !self.has_async_scrolling_state || !self.wheel_gesture_routing.admits_async_pinch() {
            return true;
        }
        let tree = self.scene.visual_context_tree_for_compositing();
        // NB: The main thread takes the wheel events of a nested document the compositor cannot reach.
        self.async_scroll_tree.blocks_wheel_event_at_position(tree, position)
            || self
                .async_scroll_tree
                .main_thread_wheel_event_region_contains(tree, position)
    }

    pub fn nested_context_for_wheel_at(&mut self, position: FloatPoint) -> Option<NestedContextHit> {
        if !self.has_async_scrolling_state {
            return None;
        }
        self.async_scroll_tree
            .nested_context_for_wheel_at(self.scene.visual_context_tree_for_compositing(), position)
    }

    pub fn latch_wheel_gesture_to_nested_context(
        &mut self,
        nested_context_id: CompositorContextId,
        scroller: Option<AsyncScrollNodeStableID>,
        scroll_gesture_phase: ScrollGesturePhase,
    ) {
        self.wheel_gesture_routing.latch_to_nested_context(
            nested_context_id,
            scroller,
            scroll_gesture_phase,
            self.wheel_gesture_routing.gesture_id(),
        );
    }

    // Where a position in the viewport of a context nested in this one lies in this context's viewport.
    pub fn position_of_nested_context_position(
        &mut self,
        nested_context_id: CompositorContextId,
        position_in_nested_context: FloatPoint,
        nested_context_viewport_size: IntSize,
    ) -> Option<FloatPoint> {
        if !self.has_async_scrolling_state {
            return None;
        }
        self.async_scroll_tree.position_of_nested_context_position(
            nested_context_id,
            position_in_nested_context,
            libgfx_rust::FloatSize {
                width: nested_context_viewport_size.width as f32,
                height: nested_context_viewport_size.height as f32,
            },
            self.scene.visual_context_tree_for_compositing(),
        )
    }

    // The scrollers the page found around the target of a step that it let through, innermost first, go before a hit
    // test at the position of the step.
    pub fn route_first_wheel_step_from_containing_context(
        &mut self,
        position: FloatPoint,
        delta_in_css_pixels: FloatPoint,
        wheel_delta_precision: WheelDeltaPrecision,
        nested_context_regions: NestedContextRegions,
        admission: WheelStepAdmission,
        scroll_chain: &[AsyncScrollNodeStableID],
    ) -> FirstWheelStepRoute {
        use FirstWheelStepRouteKind as Kind;
        let route = |kind, decision, scroll_node_index, scroller, nested_context_hit| FirstWheelStepRoute {
            kind,
            decision,
            scroll_node_index,
            scroller,
            nested_context_hit,
        };
        let routing_admission = self.wheel_gesture_routing.admission();
        if !self.has_async_scrolling_state || routing_admission == WheelRoutingAdmission::NoAsyncScrollingState {
            return route(
                Kind::Declined,
                InputRoutingDecision::NoAsyncScrollingState,
                None,
                None,
                None,
            );
        }
        // A document's wheel event listeners never see the wheel events over a document nested in it.
        let tree = self.scene.visual_context_tree_for_compositing();
        if nested_context_regions == NestedContextRegions::HandToNestedContext
            && let Some(hit) = self.async_scroll_tree.nested_context_for_wheel_at(tree, position)
        {
            return route(
                Kind::NestedContext,
                InputRoutingDecision::HandedToNestedContext,
                None,
                None,
                Some(hit),
            );
        }
        let listeners_of_this_page_see_the_step = admission == WheelStepAdmission::ByThisPage;
        if listeners_of_this_page_see_the_step
            && matches!(
                routing_admission,
                WheelRoutingAdmission::BlockingWheelEventListeners | WheelRoutingAdmission::StaleWheelEventListeners
            )
        {
            return route(
                Kind::Declined,
                input_routing_decision_for_wheel_step_not_admitted_by(routing_admission),
                None,
                None,
                None,
            );
        }
        let delta = delta_in_css_pixels.scaled(self.async_scroll_tree.device_pixels_per_css_pixel() as f32);
        let wheel_step_dispatch = if listeners_of_this_page_see_the_step {
            WheelStepDispatch::NotYetDispatched
        } else {
            WheelStepDispatch::DispatchedAndNotCanceled
        };
        let scroll_target = self.route_first_wheel_step(
            position,
            delta,
            wheel_delta_precision,
            wheel_step_dispatch,
            nested_context_regions,
            scroll_chain,
        );
        let scroller_of = |index: Option<SpatialNodeIndex>| {
            index
                .and_then(|index| self.async_scroll_tree.scroll_node_for_index(index))
                .map(|node| node.stable_node_id)
        };
        if scroll_target.outcome == WheelHitTestOutcome::FoundScrollTarget {
            return route(
                Kind::ScrollNode,
                InputRoutingDecision::Latched,
                scroll_target.scroll_node_index,
                scroller_of(scroll_target.scroll_node_index),
                None,
            );
        }
        if scroll_target.outcome != WheelHitTestOutcome::NoScrollTarget {
            return route(
                Kind::Declined,
                input_routing_decision_for_first_wheel_step(scroll_target.outcome),
                None,
                None,
                None,
            );
        }
        // https://drafts.csswg.org/css-overscroll-1/#scroll-chaining-and-boundary-default-actions
        // A nested context does not stretch at its edge. It passes the step on to the document around it unless a scroller
        // keeps it, the viewport included.
        let viewport_keeps_the_scroll = self.async_scroll_tree.viewport_keeps_scrolls_along_main_axis_of(delta);
        if scroll_target.scroll_node_index_keeping_the_scroll.is_none() && !viewport_keeps_the_scroll {
            return route(
                Kind::PassedOn,
                InputRoutingDecision::NestedContextPassedStepOn,
                None,
                None,
                None,
            );
        }
        let scroll_node_index_keeping_the_scroll = scroll_target
            .scroll_node_index_keeping_the_scroll
            .or_else(|| self.async_scroll_tree.viewport_scroll_node_index());
        route(
            Kind::Kept,
            InputRoutingDecision::ScrollKeptByOverscrollBehavior,
            scroll_node_index_keeping_the_scroll,
            scroller_of(scroll_node_index_keeping_the_scroll),
            None,
        )
    }

    // Latches the gesture to the scroller the route found, or to the one that keeps the step, and scrolls by the step.
    pub fn take_first_wheel_step_from_containing_context(
        &mut self,
        route: &FirstWheelStepRoute,
        step: &WheelStep,
        now: MonotonicTime,
    ) -> ContextUpdateResult {
        assert!(matches!(
            route.kind,
            FirstWheelStepRouteKind::ScrollNode | FirstWheelStepRouteKind::Kept
        ));
        if route.kind == FirstWheelStepRouteKind::Kept {
            if let Some(scroller) = route.scroller {
                self.wheel_gesture_routing
                    .latch(scroller, step.scroll_gesture_phase, step.gesture_id);
            } else {
                self.wheel_gesture_routing
                    .latch_keeping_steps(step.scroll_gesture_phase, step.gesture_id);
            }
            return ContextUpdateResult {
                accepted: true,
                routing_decision: Some(route.decision),
                routed_scroller: route.scroller,
                ..Default::default()
            };
        }
        let (Some(scroller), Some(scroll_node_index)) = (route.scroller, route.scroll_node_index) else {
            return ContextUpdateResult::declined(InputRoutingDecision::NestedContextUnavailable);
        };
        self.wheel_gesture_routing
            .latch(scroller, step.scroll_gesture_phase, step.gesture_id);
        self.take_wheel_step_from_containing_context(
            scroll_node_index,
            route.decision,
            step,
            ScrollChaining::ToScrollableAncestors,
            now,
        )
    }

    pub fn take_latched_wheel_step_from_containing_context(
        &mut self,
        step: &WheelStep,
        admission: WheelStepAdmission,
        now: MonotonicTime,
    ) -> ContextUpdateResult {
        if !self.has_async_scrolling_state {
            return ContextUpdateResult::declined(InputRoutingDecision::NoAsyncScrollingState);
        }
        let latch = self.wheel_gesture_routing.resolve_latch(
            &self.async_scroll_tree,
            self.scene.visual_context_tree().as_ref(),
            step.scroll_gesture_phase,
            step.gesture_id,
        );
        if latch.keeps_steps {
            return ContextUpdateResult {
                accepted: true,
                routing_decision: Some(InputRoutingDecision::ScrollKeptByOverscrollBehavior),
                ..Default::default()
            };
        }
        let Some(scroll_node_index) = latch.scroll_node_index else {
            return ContextUpdateResult::declined(InputRoutingDecision::NestedContextUnavailable);
        };
        if step.scroll_gesture_phase == ScrollGesturePhase::Ended && step.delta_in_css_pixels.is_zero() {
            self.scroll_snap_controller
                .note_gesture_phase(step.scroll_gesture_phase);
            let operation_id = self.snap_at_gesture_end(now);
            if operation_id.is_none() {
                if admission == WheelStepAdmission::LetThroughByThePage {
                    return ContextUpdateResult {
                        accepted: true,
                        routing_decision: Some(InputRoutingDecision::GestureEndWithNothingToSnap),
                        ..Default::default()
                    };
                }
                return ContextUpdateResult::declined(InputRoutingDecision::GestureEndWithNothingToSnap);
            }
            return ContextUpdateResult {
                accepted: true,
                frame_to_present: Some(PendingFrame::repainting_changes(self.async_scrolling_viewport_rect)),
                should_request_rendering_update: true,
                routing_decision: Some(InputRoutingDecision::SnappedAtGestureEnd),
                routed_scroller: self.wheel_gesture_routing.latched_scroller(),
                tracked_scroll_operation: self.tracked_scroll_operation(step.operation_tracking, operation_id),
                ..Default::default()
            };
        }
        self.take_wheel_step_from_containing_context(
            scroll_node_index,
            InputRoutingDecision::ContinuedLatch,
            step,
            ScrollChaining::None,
            now,
        )
    }

    fn take_wheel_step_from_containing_context(
        &mut self,
        scroll_node_index: SpatialNodeIndex,
        decision: InputRoutingDecision,
        step: &WheelStep,
        scroll_chaining: ScrollChaining,
        now: MonotonicTime,
    ) -> ContextUpdateResult {
        self.scroll_snap_controller
            .note_gesture_phase(step.scroll_gesture_phase);
        let delta = step
            .delta_in_css_pixels
            .scaled(self.async_scroll_tree.device_pixels_per_css_pixel() as f32);
        let outcome = self.perform_wheel_scroll_of_node(
            scroll_node_index,
            delta,
            step.wheel_delta_precision,
            step.scroll_gesture_phase,
            step.operation_tracking,
            self.async_scrolling_viewport_rect,
            scroll_chaining,
            VisualViewportPanning::No,
            now,
        );
        ContextUpdateResult {
            accepted: true,
            frame_to_present: outcome.viewport_rect_to_present.map(PendingFrame::repainting_changes),
            should_request_rendering_update: outcome.viewport_rect_to_present.is_some()
                && !outcome.only_elastic_overscroll_changed,
            routing_decision: Some(decision),
            routed_scroller: self.wheel_gesture_routing.latched_scroller(),
            tracked_scroll_operation: self.tracked_scroll_operation(step.operation_tracking, outcome.operation_id),
            ..Default::default()
        }
    }
}
