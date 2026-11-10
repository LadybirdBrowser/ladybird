/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Wheel gestures, nested scroll routing, and scroll requests for compositor contexts.

use super::input::position_in_nested_context;
use super::*;
use crate::context_state::{
    FirstWheelStepRouteKind, NestedContextWheelStep, TrackedScrollOperation, WheelEventListenerGate, WheelStep,
    WheelStepAdmission,
};
use libcompositing_rust::ipc_types::{
    Compositing_AsyncScrollOperationTracking as AsyncScrollOperationTracking,
    Compositing_SmoothScrollInitiator as SmoothScrollInitiator, Compositing_WheelEventAnswer as WheelEventAnswer,
    Compositing_WheelEventRouting as WheelEventRouting, Compositing_WheelStepLetThrough as WheelStepLetThrough,
    Web_InputTraceRecord as InputTraceRecord, Web_InputTraceStage as InputTraceStage,
};
use libcompositing_rust::scrolling::{
    AsyncScrollNodeStableID, InputRoutingDecision, KeyboardScrollState, MouseEvent, NestedContextHit,
    NestedContextRegions, PendingAsyncScrollUpdates, ScrollGesturePhase, ScrollRequest, WheelDeltaPrecision,
    WheelGestureMark,
};

impl CompositorCore {
    pub fn update_scroll_state(
        &self,
        context_id: CompositorContextId,
        scroll_state_snapshot: ScrollStateSnapshot,
        keyboard_scroll_state: KeyboardScrollState,
    ) {
        let _entered = self.enter();

        let context = self.context_if_present(context_id).expect("context exists");

        context
            .borrow_mut()
            .update_scroll_state(scroll_state_snapshot, keyboard_scroll_state);
    }

    pub fn invalidate_wheel_event_listener_state(&self, context_id: CompositorContextId, generation: u64) {
        let _entered = self.enter();

        let context = self.context_if_present(context_id).expect("context exists");
        context.borrow_mut().invalidate_wheel_event_listener_state(generation);
    }

    pub fn invalidate_keyboard_scroll_state(&self, context_id: CompositorContextId, generation: u64) {
        let _entered = self.enter();

        if let Some(context) = self.context_if_present(context_id) {
            context.borrow_mut().invalidate_keyboard_scroll_state(generation);
        }
    }

    // A fling step is scrolled at the frame time the fling synthesized it for.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn scroll_for_wheel_step(
        &self,
        context_id: CompositorContextId,
        context: &RefCell<ContextState>,
        event: &MouseEvent,
        origin: WheelStepOrigin,
        is_first_wheel_event_with_delta: bool,
        operation_tracking: AsyncScrollOperationTracking,
        now: MonotonicTime,
    ) -> ContextUpdateResult {
        let gesture_id = context.borrow().wheel_gesture_id();
        let listener_context_id = context.borrow().wheel_event_listener_context().unwrap_or(context_id);
        let listener_context = if listener_context_id == context_id {
            Some(context)
        } else {
            self.context_still_nested_in(context_id, Some(listener_context_id))
        };
        let listener_gate = if let Some(listener_context) = listener_context {
            listener_context.borrow().wheel_event_listener_gate(gesture_id)
        } else {
            WheelEventListenerGate {
                reason_to_hold_latched_steps_back: Some(InputRoutingDecision::NestedContextUnavailable),
                ..Default::default()
            }
        };

        let mut result;
        let mut presents_frame;
        let route_from_listener = listener_context
            .is_some_and(|listener_context| !std::ptr::eq(listener_context, context))
            && listener_gate.page_let_the_gesture_through
            && !context.borrow().has_wheel_latch_for_gesture(gesture_id);
        if route_from_listener {
            let listener_context = listener_context.unwrap();
            // The page of a nested context let the gesture through while nothing took its steps, so they go to that context
            // again rather than to the page.
            let mut step = WheelStep::for_wheel_event(event, gesture_id, operation_tracking);
            let position =
                self.position_in_nested_context_ignoring_clips(context_id, context, listener_context, step.position);
            let Some(position) = position else {
                return ContextUpdateResult::declined(InputRoutingDecision::NestedContextUnavailable);
            };
            step.position = position;
            let applied_step = self.route_wheel_step_the_page_let_through(
                context_id,
                context,
                listener_context_id,
                listener_context,
                step,
                &[],
                None,
                now,
            );
            result = applied_step.result;
            result.nested_context_hand_off = Some(InputRoutingDecision::HandedToNestedContext);
            presents_frame = applied_step.presents_frame;
        } else {
            result = match origin {
                WheelStepOrigin::UI => {
                    context
                        .borrow_mut()
                        .handle_wheel_event(event, is_first_wheel_event_with_delta, &listener_gate, now)
                }
                WheelStepOrigin::Fling => context.borrow_mut().scroll_for_fling_step(event, &listener_gate, now),
                WheelStepOrigin::MainThread => context.borrow_mut().scroll_for_wheel_event_from_main_thread(
                    event,
                    is_first_wheel_event_with_delta,
                    &listener_gate,
                    operation_tracking,
                    now,
                ),
            };
            presents_frame = result.frame_to_present.is_some();
            if let Some(nested_step) = &result.nested_context_wheel_step {
                let applied_step = self.hand_wheel_step_to_nested_context(
                    context_id,
                    context,
                    event,
                    nested_step,
                    &listener_gate,
                    operation_tracking,
                    now,
                );
                result = applied_step.result;
                presents_frame = applied_step.presents_frame;
            }
        }

        // If the compositor accepts a momentum step of the fling but does not scroll, the scroller is at its edge.
        // Alternatively, a snap scroll uses the step. The next display tick ends the fling.
        if origin == WheelStepOrigin::Fling
            && event.scroll_gesture_phase == ScrollGesturePhase::Momentum
            && result.accepted
            && !presents_frame
        {
            context.borrow_mut().end_scroll_fling_on_next_step();
        }
        result
    }

    // A process only knows the ids of its own nodes, and only follows the operations of its own contexts.
    fn apply_wheel_step_result(
        &self,
        context_id: CompositorContextId,
        context: &RefCell<ContextState>,
        result: &ContextUpdateResult,
        reporting_client: WebContentClientId,
        now: MonotonicTime,
    ) -> AppliedWheelStep {
        self.apply_context_update_result(context_id, context, result);
        context.borrow_mut().request_display_ticks_if_needed(now);
        // Node ids are only unique within the process that made them, and only that process follows the operations of the
        // context.
        let ids_are_of_reporting_process = context.borrow().is_owned_by(reporting_client);
        AppliedWheelStep {
            result: ContextUpdateResult {
                accepted: result.accepted,
                routing_decision: result.routing_decision,
                routed_scroller: if ids_are_of_reporting_process {
                    result.routed_scroller
                } else {
                    None
                },
                tracked_scroll_operation: if ids_are_of_reporting_process {
                    result
                        .tracked_scroll_operation
                        .as_ref()
                        .map(|operation| TrackedScrollOperation {
                            context_id: operation.context_id,
                            operation_id: operation.operation_id,
                        })
                } else {
                    None
                },
                ..Default::default()
            },
            presents_frame: result.frame_to_present.is_some(),
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn hand_wheel_step_to_nested_context(
        &self,
        context_id: CompositorContextId,
        context: &RefCell<ContextState>,
        event: &MouseEvent,
        nested_step: &NestedContextWheelStep,
        listener_gate: &WheelEventListenerGate,
        operation_tracking: AsyncScrollOperationTracking,
        now: MonotonicTime,
    ) -> AppliedWheelStep {
        let mut step = WheelStep::for_wheel_event(event, context.borrow().wheel_gesture_id(), operation_tracking);

        if nested_step.hit.is_none() {
            let nested_context = self.context_if_present(nested_step.context_id);
            if nested_context.is_none_or(|context| !self.is_nested_in(context_id, context)) {
                context.borrow_mut().forget_wheel_gesture_latch();
                return AppliedWheelStep {
                    result: ContextUpdateResult::declined(InputRoutingDecision::NestedContextUnavailable),
                    ..Default::default()
                };
            }
            let nested_context = nested_context.unwrap();
            // A blocking listener added during the gesture holds its later steps back until the page lets one through.
            if let Some(reason) = listener_gate.reason_to_hold_latched_steps_back {
                return AppliedWheelStep {
                    result: ContextUpdateResult::declined(reason),
                    ..Default::default()
                };
            }
            let admission = if listener_gate.page_let_the_gesture_through {
                WheelStepAdmission::LetThroughByThePage
            } else {
                WheelStepAdmission::ByThisPage
            };
            let nested_result = nested_context
                .borrow_mut()
                .take_latched_wheel_step_from_containing_context(&step, admission, now);
            if nested_result.routing_decision == Some(InputRoutingDecision::NestedContextUnavailable) {
                context.borrow_mut().forget_wheel_gesture_latch();
            }
            let reporting_client = context.borrow().web_content_client_id();
            return self.apply_wheel_step_result(
                nested_step.context_id,
                nested_context,
                &nested_result,
                reporting_client,
                now,
            );
        }

        let first_step = self.first_wheel_step_in_nested_contexts(
            context_id,
            {
                let hit = nested_step.hit.as_ref().unwrap();
                NestedContextHit {
                    child_context_id: hit.child_context_id,
                    position_in_content_box: hit.position_in_content_box,
                    content_box_size: hit.content_box_size,
                    paint_order_index: hit.paint_order_index,
                }
            },
            step.delta_in_css_pixels,
            step.wheel_delta_precision,
        );
        context
            .borrow_mut()
            .set_wheel_event_listener_context(first_step.innermost_context_id);
        let Some(taking_context) = first_step.taking_context else {
            let mut result =
                context
                    .borrow_mut()
                    .scroll_for_wheel_event_passed_on_by_nested_context(event, operation_tracking, now);
            result.nested_context_hand_off = Some(InputRoutingDecision::NestedContextPassedStepOn);
            let presents_frame = result.frame_to_present.is_some();
            return AppliedWheelStep { result, presents_frame };
        };

        let mut applied_step;
        if let Some(nested_context) = taking_context.context
            && taking_context.route.kind != FirstWheelStepRouteKind::Declined
        {
            step.position = taking_context.position;
            let nested_result = nested_context
                .borrow_mut()
                .take_first_wheel_step_from_containing_context(&taking_context.route, &step, now);
            let reporting_client = context.borrow().web_content_client_id();
            applied_step = self.apply_wheel_step_result(
                taking_context.context_id,
                nested_context,
                &nested_result,
                reporting_client,
                now,
            );
            context.borrow_mut().latch_wheel_gesture_to_nested_context(
                taking_context.context_id,
                applied_step.result.routed_scroller,
                step.scroll_gesture_phase,
            );
        } else {
            applied_step = AppliedWheelStep {
                result: ContextUpdateResult::declined(taking_context.route.decision),
                ..Default::default()
            };
        }
        applied_step.result.nested_context_hand_off = Some(InputRoutingDecision::HandedToNestedContext);
        applied_step
    }

    // Where the first step of a gesture over the content of a context nested in the context `context_id` goes, among
    // that context and the contexts nested in it.
    pub(super) fn first_wheel_step_in_nested_contexts(
        &self,
        context_id: CompositorContextId,
        mut hit: NestedContextHit,
        delta_in_css_pixels: FloatPoint,
        wheel_delta_precision: WheelDeltaPrecision,
    ) -> FirstWheelStepInNestedContexts<'_> {
        use FirstWheelStepRouteKind as Kind;
        // The contexts the step went into, outermost first, each nested in the one before.
        let mut contexts = Vec::new();
        let mut containing_context_id = context_id;
        loop {
            let declined_by_unavailable_context = || FirstWheelStepInNestedContexts {
                innermost_context_id: hit.child_context_id,
                taking_context: Some(NestedContextTakingWheelStep {
                    context_id: hit.child_context_id,
                    context: None,
                    position: FloatPoint::default(),
                    route: FirstWheelStepRoute {
                        kind: Kind::Declined,
                        decision: InputRoutingDecision::NestedContextUnavailable,
                        scroll_node_index: None,
                        scroller: None,
                        nested_context_hit: None,
                    },
                }),
            };
            // WebContent names the contexts its display lists draw, so a context only reaches those nested in it.
            let Some(nested_context) = self.context_if_present(hit.child_context_id) else {
                return declined_by_unavailable_context();
            };
            if nested_context.borrow().parent_context_id() != Some(containing_context_id) {
                return declined_by_unavailable_context();
            }
            let Some(position) = position_in_nested_context(&hit, nested_context) else {
                return declined_by_unavailable_context();
            };
            let route = nested_context
                .borrow_mut()
                .route_first_wheel_step_from_containing_context(
                    position,
                    delta_in_css_pixels,
                    wheel_delta_precision,
                    NestedContextRegions::HandToNestedContext,
                    WheelStepAdmission::ByThisPage,
                    &[],
                );
            let kind = route.kind;
            let nested_context_hit = route.nested_context_hit.as_ref().map(|hit| NestedContextHit {
                child_context_id: hit.child_context_id,
                position_in_content_box: hit.position_in_content_box,
                content_box_size: hit.content_box_size,
                paint_order_index: hit.paint_order_index,
            });
            contexts.push(NestedContextTakingWheelStep {
                context_id: hit.child_context_id,
                context: Some(nested_context),
                position,
                route,
            });
            if kind != Kind::NestedContext {
                break;
            }
            containing_context_id = hit.child_context_id;
            hit = nested_context_hit.unwrap();
        }

        // What a nested context passes on goes to whatever is painted under it in the context it is nested in, whose page
        // does not see the step.
        let innermost_context_id = contexts.last().unwrap().context_id;
        while contexts.last().unwrap().route.kind == Kind::PassedOn {
            contexts.pop();
            if contexts.is_empty() {
                return FirstWheelStepInNestedContexts {
                    innermost_context_id,
                    taking_context: None,
                };
            }
            let containing_context = contexts.last_mut().unwrap();
            containing_context.route = containing_context
                .context
                .unwrap()
                .borrow_mut()
                .route_first_wheel_step_from_containing_context(
                    containing_context.position,
                    delta_in_css_pixels,
                    wheel_delta_precision,
                    NestedContextRegions::Ignore,
                    WheelStepAdmission::PassedOnByNestedContext,
                    &[],
                );
        }
        FirstWheelStepInNestedContexts {
            innermost_context_id,
            taking_context: contexts.pop(),
        }
    }
}

// The page of one process scrolls a context of another process only by what the UI left to the page in the gesture, so
// that a page cannot scroll the document around it at will.
fn page_may_scroll_in(
    context: &RefCell<ContextState>,
    routing_context: &RefCell<ContextState>,
    page_client: Option<WebContentClientId>,
    step: &WheelStep,
) -> bool {
    let Some(page_client) = page_client else { return true };
    if context.borrow().is_owned_by(page_client) {
        return true;
    }
    routing_context
        .borrow_mut()
        .take_wheel_delta_left_to_the_page(step.delta_in_css_pixels)
}

impl CompositorCore {
    // Routes the first step of a gesture that the page of the listener context let through: in that context, from the
    // scrollers the page found around the target, and on to the contexts around it for what a context passes on, up to
    // the routing context, which latches the gesture. A page of another process than a context's scrolls it only by
    // what the UI left to the page in the gesture.
    #[allow(clippy::too_many_arguments)]
    fn route_wheel_step_the_page_let_through(
        &self,
        routing_context_id: CompositorContextId,
        routing_context: &RefCell<ContextState>,
        listener_context_id: CompositorContextId,
        listener_context: &RefCell<ContextState>,
        mut step: WheelStep,
        mut scroll_chain: &[AsyncScrollNodeStableID],
        page_client: Option<WebContentClientId>,
        now: MonotonicTime,
    ) -> AppliedWheelStep {
        use FirstWheelStepRouteKind as Kind;
        let reporting_client = page_client.unwrap_or_else(|| routing_context.borrow().web_content_client_id());
        let declined = |decision| AppliedWheelStep {
            result: ContextUpdateResult::declined(decision),
            ..Default::default()
        };

        let mut context_id = listener_context_id;
        let mut context = listener_context;
        // A walk up the parents that outlasts the number of contexts went around a loop.
        for _ in 0..self.contexts.len() {
            if std::ptr::eq(context, routing_context) {
                if !page_may_scroll_in(routing_context, routing_context, page_client, &step) {
                    return declined(InputRoutingDecision::StepExceedsInputLeftToThePage);
                }
                let result = routing_context
                    .borrow_mut()
                    .scroll_for_first_wheel_step_the_page_let_through(&step, scroll_chain, now);
                return self.apply_wheel_step_result(
                    routing_context_id,
                    routing_context,
                    &result,
                    reporting_client,
                    now,
                );
            }

            let route = context.borrow_mut().route_first_wheel_step_from_containing_context(
                step.position,
                step.delta_in_css_pixels,
                step.wheel_delta_precision,
                NestedContextRegions::Ignore,
                WheelStepAdmission::LetThroughByThePage,
                scroll_chain,
            );
            if matches!(route.kind, Kind::ScrollNode | Kind::Kept) {
                if !page_may_scroll_in(context, routing_context, page_client, &step) {
                    return declined(InputRoutingDecision::StepExceedsInputLeftToThePage);
                }
                let nested_result = context
                    .borrow_mut()
                    .take_first_wheel_step_from_containing_context(&route, &step, now);
                let client = routing_context.borrow().web_content_client_id();
                let latched_scroller = if context.borrow().is_owned_by(client) {
                    nested_result.routed_scroller
                } else {
                    None
                };
                routing_context.borrow_mut().latch_wheel_gesture_to_nested_context(
                    context_id,
                    latched_scroller,
                    step.scroll_gesture_phase,
                );
                return self.apply_wheel_step_result(context_id, context, &nested_result, reporting_client, now);
            }
            if route.kind != Kind::PassedOn {
                return declined(route.decision);
            }

            // What a nested context passes on goes to whatever is painted under it in the context it is nested in.
            let parent_context_id = context.borrow().parent_context_id();
            let parent_context = parent_context_id.and_then(|id| self.context_if_present(id));
            let Some(parent_context) = parent_context else {
                return declined(InputRoutingDecision::NestedContextUnavailable);
            };
            let viewport_size = context.borrow().viewport_size();
            let position = parent_context.borrow_mut().position_of_nested_context_position(
                context_id,
                step.position,
                viewport_size,
            );
            let Some(position) = position else {
                return declined(InputRoutingDecision::NestedContextUnavailable);
            };
            step.position = position;
            scroll_chain = &[];
            context_id = parent_context_id.unwrap();
            context = parent_context;
        }
        declined(InputRoutingDecision::NestedContextUnavailable)
    }

    // The nearest context, from the given one out to the presenting one, that noted the gesture.
    fn routing_context_of_wheel_gesture(
        &self,
        context_id: CompositorContextId,
        gesture_id: u64,
    ) -> Option<CompositorContextId> {
        // A walk up the parents that outlasts the number of contexts went around a loop.
        let mut candidate_context_id = Some(context_id);
        for _ in 0..self.contexts.len() {
            let id = candidate_context_id?;
            let candidate_context = self.context_if_present(id)?;
            if candidate_context.borrow().wheel_gesture_id() == Some(gesture_id) {
                return candidate_context_id;
            }
            candidate_context_id = candidate_context.borrow().parent_context_id();
        }
        None
    }

    pub(super) fn dispatch_scroll_fling_step(
        &self,
        context_id: CompositorContextId,
        context: &RefCell<ContextState>,
        event: Option<MouseEvent>,
        now: MonotonicTime,
    ) {
        // Send each fling step on the same route as a wheel event from the UI.
        let Some(mut event) = event else { return };
        let result = self.scroll_for_wheel_step(
            context_id,
            context,
            &event,
            WheelStepOrigin::Fling,
            false,
            AsyncScrollOperationTracking::No,
            now,
        );
        if let Some(decision) = result.nested_context_hand_off {
            context.borrow_mut().input_trace_mut().record(InputTraceRecord {
                input_event_id: event.id,
                stage: InputTraceStage::Compositor,
                decision,
                scroller: None,
                dom_target: None,
            });
        }
        if let Some(decision) = result.routing_decision {
            context.borrow_mut().input_trace_mut().record(InputTraceRecord {
                input_event_id: event.id,
                stage: InputTraceStage::Compositor,
                decision,
                scroller: result.routed_scroller,
                dom_target: None,
            });
        }
        event.async_scroll_performed_default_action = self.apply_context_update_result(context_id, context, &result);
        if !event.async_scroll_performed_default_action {
            context.borrow_mut().note_wheel_event_left_to_the_page(&event);
        }
        context.borrow_mut().request_display_ticks_if_needed(now);
        context.borrow().dispatch_mouse_event_to_web_content(&event, false);
    }

    fn wheel_event_answer_for(
        &self,
        applied_step: &AppliedWheelStep,
        wheel_gesture_mark: Option<WheelGestureMark>,
    ) -> WheelEventAnswer {
        let tracked_scroll_operation = &applied_step.result.tracked_scroll_operation;
        WheelEventAnswer {
            performed_default_action: applied_step.result.accepted,
            wheel_gesture_mark,
            operation_id: tracked_scroll_operation
                .as_ref()
                .map(|operation| operation.operation_id),
            operation_context_id: tracked_scroll_operation.as_ref().map(|operation| operation.context_id),
            routing_decision: applied_step.result.routing_decision,
            routed_scroller: applied_step.result.routed_scroller,
        }
    }

    pub fn take_wheel_event_from_main_thread(
        &self,
        context_id: CompositorContextId,
        event: &MouseEvent,
        operation_tracking: AsyncScrollOperationTracking,
        routing: WheelEventRouting,
        now: MonotonicTime,
    ) -> WheelEventAnswer {
        let _entered = self.enter();

        let context = self.context_if_present(context_id).expect("context exists");

        let noted = context.borrow_mut().note_wheel_event_from_main_thread(event, now);
        if routing == WheelEventRouting::AfterDispatch {
            return WheelEventAnswer {
                performed_default_action: false,
                wheel_gesture_mark: Some(noted.wheel_gesture_mark),
                operation_id: None,
                operation_context_id: None,
                routing_decision: None,
                routed_scroller: None,
            };
        }
        let result = self.scroll_for_wheel_step(
            context_id,
            context,
            event,
            WheelStepOrigin::MainThread,
            noted.is_first_wheel_event_with_delta,
            operation_tracking,
            now,
        );
        let reporting_client = context.borrow().web_content_client_id();
        self.wheel_event_answer_for(
            &self.apply_wheel_step_result(context_id, context, &result, reporting_client, now),
            Some(noted.wheel_gesture_mark),
        )
    }

    pub fn scroll_for_wheel_event_the_page_let_through(
        &self,
        listener_context_id: CompositorContextId,
        let_through: &WheelStepLetThrough,
        operation_tracking: AsyncScrollOperationTracking,
        page_client: WebContentClientId,
        now: MonotonicTime,
    ) -> WheelEventAnswer {
        let _entered = self.enter();

        let listener_context = self.context_if_present(listener_context_id).expect("context exists");
        if listener_context.borrow().document_id() != Some(let_through.document_id) {
            return WheelEventAnswer {
                performed_default_action: false,
                wheel_gesture_mark: None,
                operation_id: None,
                operation_context_id: None,
                routing_decision: Some(InputRoutingDecision::ScrollerInOtherDocument),
                routed_scroller: None,
            };
        }

        let gesture_id = let_through.wheel_gesture_mark.gesture_id;
        // A step that moves along no axis goes to the page as no wheel event, so the page had no say in letting it through,
        // and it decides nothing for the steps after it.
        if !let_through.delta_in_css_pixels.is_zero() {
            listener_context
                .borrow_mut()
                .note_page_let_wheel_gesture_through(gesture_id);
        }
        let step = WheelStep {
            position: let_through.position,
            delta_in_css_pixels: let_through.delta_in_css_pixels,
            wheel_delta_precision: let_through.wheel_delta_precision,
            scroll_gesture_phase: let_through.scroll_gesture_phase,
            gesture_id: Some(gesture_id),
            operation_tracking,
        };
        let scroll_chain = let_through.scroll_chain.as_slice();
        let answer = |applied_step: &AppliedWheelStep| {
            self.wheel_event_answer_for(applied_step, Some(let_through.wheel_gesture_mark.clone()))
        };
        let declined = |decision| {
            answer(&AppliedWheelStep {
                result: ContextUpdateResult::declined(decision),
                ..Default::default()
            })
        };

        // A step of a gesture that the contexts around the page have moved past still scrolls, as the page let it.
        let routing_context_id = self.routing_context_of_wheel_gesture(listener_context_id, gesture_id);
        let routing_context = routing_context_id.and_then(|id| self.context_if_present(id));
        if routing_context.is_none_or(|context| {
            context
                .borrow()
                .wheel_step_was_overtaken(step.scroll_gesture_phase, let_through.wheel_gesture_mark.step_number)
        }) {
            let result = listener_context
                .borrow_mut()
                .scroll_for_overtaken_wheel_step(&step, scroll_chain, now);
            return answer(&self.apply_wheel_step_result(
                listener_context_id,
                listener_context,
                &result,
                page_client,
                now,
            ));
        }
        let routing_context = routing_context.unwrap();
        let routing_context_id = routing_context_id.unwrap();

        routing_context
            .borrow_mut()
            .set_wheel_event_listener_context(listener_context_id);
        // The end of a gesture lets go of the scroller it stretched.
        if step.scroll_gesture_phase == ScrollGesturePhase::Ended {
            routing_context.borrow_mut().end_wheel_gesture_stretch(now);
        }

        // A latched scroller that the page no longer has around the target of the step is gone from the page, and the step
        // is routed afresh.
        let latch_is_to_scroller_of_this_page = routing_context_id == listener_context_id
            && routing_context.borrow().nested_context_of_wheel_latch().is_none();
        if latch_is_to_scroller_of_this_page && !scroll_chain.is_empty() {
            let latched_scroller = routing_context.borrow().wheel_latched_scroller();
            if let Some(latched_scroller) = latched_scroller
                && !scroll_chain.contains(&latched_scroller)
            {
                routing_context.borrow_mut().forget_wheel_latch_of_gesture(gesture_id);
            }
        }

        let has_latch = routing_context.borrow().has_wheel_latch_for_gesture(Some(gesture_id));
        if has_latch {
            let nested_context_id = routing_context.borrow().nested_context_of_wheel_latch();
            if let Some(nested_context_id) = nested_context_id {
                let nested_context = self.context_still_nested_in(routing_context_id, Some(nested_context_id));
                let Some(nested_context) = nested_context else {
                    routing_context.borrow_mut().forget_wheel_latch_of_gesture(gesture_id);
                    return declined(InputRoutingDecision::NestedContextUnavailable);
                };
                if !page_may_scroll_in(nested_context, routing_context, Some(page_client), &step) {
                    return declined(InputRoutingDecision::StepExceedsInputLeftToThePage);
                }
                let nested_result = nested_context
                    .borrow_mut()
                    .take_latched_wheel_step_from_containing_context(
                        &step,
                        WheelStepAdmission::LetThroughByThePage,
                        now,
                    );
                return answer(&self.apply_wheel_step_result(
                    nested_context_id,
                    nested_context,
                    &nested_result,
                    page_client,
                    now,
                ));
            }
            if !page_may_scroll_in(routing_context, routing_context, Some(page_client), &step) {
                return declined(InputRoutingDecision::StepExceedsInputLeftToThePage);
            }
            let result = routing_context
                .borrow_mut()
                .scroll_for_latched_wheel_step_the_page_let_through(&step, now);
            if let Some(result) = result {
                return answer(&self.apply_wheel_step_result(
                    routing_context_id,
                    routing_context,
                    &result,
                    page_client,
                    now,
                ));
            }
        }

        answer(&self.route_wheel_step_the_page_let_through(
            routing_context_id,
            routing_context,
            listener_context_id,
            listener_context,
            step,
            scroll_chain,
            Some(page_client),
            now,
        ))
    }

    pub fn forget_wheel_gesture_the_page_canceled(&self, listener_context_id: CompositorContextId, gesture_id: u64) {
        let _entered = self.enter();

        let Some(listener_context) = self.context_if_present(listener_context_id) else {
            return;
        };
        listener_context
            .borrow_mut()
            .forget_page_let_wheel_gesture_through(gesture_id);
        // The listener scrolls by itself, so the next step of the gesture is routed afresh.
        if let Some(routing_context_id) = self.routing_context_of_wheel_gesture(listener_context_id, gesture_id) {
            self.context_if_present(routing_context_id)
                .expect("routing context exists")
                .borrow_mut()
                .forget_wheel_latch_of_gesture(gesture_id);
        }
    }
}

fn scroll_request_smoothly_scrolls_for_script(scroll_request: &ScrollRequest) -> bool {
    scroll_request
        .smooth_scroll
        .as_ref()
        .is_some_and(|scroll| scroll.initiator == SmoothScrollInitiator::Programmatic)
}

impl CompositorCore {
    pub fn apply_scroll_requests(
        &self,
        context_id: CompositorContextId,
        scroll_requests: &[ScrollRequest],
        now: MonotonicTime,
    ) {
        let _entered = self.enter();

        let Some(context) = self.context_if_present(context_id) else {
            return;
        };

        // A script's smooth scroll of a box ends the momentum scrolling it, as a key's does. A script's instant scroll only
        // moves the box, and the momentum goes on from there: pages keep their content in place under a flick that way.
        for scroll_request in scroll_requests {
            if scroll_request.takes_over_from_momentum || scroll_request_smoothly_scrolls_for_script(scroll_request) {
                let event = context
                    .borrow_mut()
                    .end_momentum_scrolling(scroll_request.stable_node_id);
                self.dispatch_scroll_fling_step(context_id, context, event, now);
            }
        }
        let frame_to_present = context.borrow_mut().apply_scroll_requests(scroll_requests, now);
        if let Some(frame_to_present) = frame_to_present {
            self.request_present(context_id, &frame_to_present);
        }
        context.borrow_mut().request_display_ticks_if_needed(now);
        self.publish_pending_async_scroll_updates(context_id, context);
    }

    pub fn take_pending_async_scroll_updates(&self, context_id: CompositorContextId) -> PendingAsyncScrollUpdates {
        let _entered = self.enter();

        let context = self.context_if_present(context_id).expect("context exists");

        context.borrow_mut().take_pending_async_scroll_updates()
    }
}
