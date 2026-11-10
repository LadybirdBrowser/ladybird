/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Keyboard, mouse, and pinch input routing between compositor contexts.

use super::*;
use crate::context_state::PointerInNestedContexts;
use libcompositing_rust::ipc_types::{
    Compositing_AsyncScrollOperationTracking as AsyncScrollOperationTracking, Web_InputTraceRecord as InputTraceRecord,
    Web_InputTraceStage as InputTraceStage, compositor_presentation_client,
};
use libcompositing_rust::scrolling::{
    InputRoutingDecision, KeyEvent, KeyEventType, MouseEvent, MouseEventType, NestedContextHit, PinchEvent,
    ScrollGesturePhase,
};

impl CompositorCore {
    pub fn handle_and_dispatch_key_event(
        &self,
        context_id: CompositorContextId,
        mut event: KeyEvent,
        allow_keyboard_scroll: bool,
        now: MonotonicTime,
    ) {
        let _entered = self.enter();

        // NB: The presentation connection only passes on input for the contexts bound to it.
        let context = self.context_if_present(context_id).expect("context exists");
        assert!(context.borrow().bound_presentation().is_some());
        let presentation_id = context.borrow().bound_presentation().unwrap();
        if !context.borrow().can_dispatch_input_to_web_content() {
            // No context could take the event; the UI sends it to WebContent itself.
            self.host.post_to_presentation(
                presentation_id,
                compositor_presentation_client::messages::DidNotDispatchInputEvent {
                    context_id,
                    event_id: event.id,
                },
            );
            return;
        }

        let mut later_keys_need_not_wait = false;
        if allow_keyboard_scroll {
            let scroll_box = context.borrow().box_scrolled_for_key_event(&event);
            if let Some(scroll_box) = scroll_box {
                let end_event = context.borrow_mut().end_momentum_scrolling(scroll_box);
                self.dispatch_scroll_fling_step(context_id, context, end_event, now);
            }
            let result = context.borrow_mut().handle_key_event(&event, now);
            later_keys_need_not_wait =
                self.apply_context_update_result(context_id, context, &result) && event.r#type == KeyEventType::KeyDown;
            event.async_scroll_performed_default_action =
                later_keys_need_not_wait && !result.key_found_nothing_to_scroll;
            if later_keys_need_not_wait && result.key_found_nothing_to_scroll {
                event.user_input_scroll_count_with_nothing_to_scroll = Some(context.borrow().user_input_scroll_count());
            }
        }

        // The scroll updates the event produced reach WebContent ahead of the event itself on the same connection.
        self.publish_pending_async_scroll_updates(context_id, context);
        context.borrow().dispatch_key_event_to_web_content(&event);
        // The compositor forwarded the key event to WebContent, after performing its default action by scrolling if it says so.
        self.host.post_to_presentation(
            presentation_id,
            compositor_presentation_client::messages::DidDispatchInputEvent {
                context_id,
                event_id: event.id,
                performed_default_action: later_keys_need_not_wait,
            },
        );
    }

    pub fn handle_and_dispatch_mouse_event(
        &self,
        context_id: CompositorContextId,
        mut event: MouseEvent,
        now: MonotonicTime,
    ) {
        let _entered = self.enter();

        // NB: The presentation connection only passes on input for the contexts bound to it.
        let context = self.context_if_present(context_id).expect("context exists");
        assert!(context.borrow().bound_presentation().is_some());
        let presentation_id = context.borrow().bound_presentation().unwrap();
        let record_routing = |decision, scroller| {
            context.borrow_mut().input_trace_mut().record(InputTraceRecord {
                input_event_id: event.id,
                stage: InputTraceStage::Compositor,
                decision,
                scroller,
                dom_target: None,
            });
        };
        if !context.borrow().can_dispatch_input_to_web_content() {
            record_routing(InputRoutingDecision::NotDispatchedByCompositor, None);
            self.host.post_to_presentation(
                presentation_id,
                compositor_presentation_client::messages::DidNotDispatchInputEvent {
                    context_id,
                    event_id: event.id,
                },
            );
            return;
        }

        if event.r#type == MouseEventType::MouseMove {
            context.borrow_mut().note_pointer_position(Some(FloatPoint {
                x: event.position.x as f32,
                y: event.position.y as f32,
            }));
        } else if matches!(event.r#type, MouseEventType::MouseLeave | MouseEventType::MouseCancel) {
            context.borrow_mut().note_pointer_position(None);
        }

        if event.r#type == MouseEventType::MouseDown {
            let end_event = context.borrow_mut().end_scroll_fling();
            self.dispatch_scroll_fling_step(context_id, context, end_event, now);
        }

        let is_wheel_event = event.r#type == MouseEventType::MouseWheel;
        let mut is_first_wheel_event_with_delta = false;
        if is_wheel_event {
            // A key, or a script's smooth scroll, takes over from momentum, until wheel input other than the momentum takes
            // over again.
            let absorbs_event = context.borrow_mut().absorbs_wheel_event_of_interrupted_momentum(&event);
            if absorbs_event {
                record_routing(InputRoutingDecision::MomentumInterrupted, None);
                // The compositor performed the event's whole default action, so the UI hears nothing more about it.
                self.host.post_to_presentation(
                    presentation_id,
                    compositor_presentation_client::messages::DidConsumeInputEvent {
                        context_id,
                        event_id: event.id,
                    },
                );
                return;
            }
            let noted = context.borrow_mut().note_wheel_event_from_ui(&event, now);
            event.wheel_gesture_mark = Some(noted.wheel_gesture_mark);
            is_first_wheel_event_with_delta = noted.is_first_wheel_event_with_delta;

            // The UI can send the end of a gesture more than once. An end that arrives during a fling belongs to the
            // flicked gesture, so it does not stop the fling.
            if event.scroll_gesture_phase == ScrollGesturePhase::Ended && context.borrow().has_active_scroll_fling() {
                record_routing(InputRoutingDecision::GestureEndAbsorbedByFling, None);
                self.host.post_to_presentation(
                    presentation_id,
                    compositor_presentation_client::messages::DidConsumeInputEvent {
                        context_id,
                        event_id: event.id,
                    },
                );
                return;
            }

            // A new gesture must not continue the gesture of the fling, so the fling ends first.
            let end_event = context.borrow_mut().interrupt_scroll_fling(&event, now);
            self.dispatch_scroll_fling_step(context_id, context, end_event, now);

            // If the gesture continues as a fling, the end of the fling is the end of the gesture.
            let synthesizes_scroll_momentum = context.borrow().presentation_synthesizes_scroll_momentum();
            let starts_fling = synthesizes_scroll_momentum
                && event.scroll_gesture_phase == ScrollGesturePhase::Ended
                && context.borrow_mut().start_scroll_fling_if_flicked(&event, now);
            if starts_fling {
                record_routing(InputRoutingDecision::FlingStarted, None);
                self.host.post_to_presentation(
                    presentation_id,
                    compositor_presentation_client::messages::DidConsumeInputEvent {
                        context_id,
                        event_id: event.id,
                    },
                );
                context.borrow_mut().request_display_ticks_if_needed(now);
                return;
            }
        }

        let result = if is_wheel_event {
            self.scroll_for_wheel_step(
                context_id,
                context,
                &event,
                WheelStepOrigin::UI,
                is_first_wheel_event_with_delta,
                AsyncScrollOperationTracking::No,
                now,
            )
        } else {
            self.handle_mouse_event_in_nested_contexts(context_id, context, &event, now)
        };
        if let Some(decision) = result.nested_context_hand_off {
            record_routing(decision, None);
        }
        if let Some(decision) = result.routing_decision {
            record_routing(decision, result.routed_scroller);
        }

        // Asks for the present, publishes the scroll updates the event produced and asks for a rendering update, the last
        // two of which reach WebContent ahead of the event itself on the same connection. Those of a nested context
        // another process owns go on that process's connection instead, while the event reaches it through the UI.
        let handled = self.apply_context_update_result(context_id, context, &result);
        // A wheel event that lets go of a stretched scroller presents nothing itself; the rebound does from here on. A press
        // beside the thumb of a scrollbar scrolls there in an animation too.
        context.borrow_mut().request_display_ticks_if_needed(now);
        if is_wheel_event {
            event.async_scroll_performed_default_action = handled;
            if !handled {
                context.borrow_mut().note_wheel_event_left_to_the_page(&event);
            }
        } else if handled {
            // The page still sees the events of a drag the compositor scrolls for a scrollbar the display list paints.
            if result.scrollbar_dragged_by_compositor.is_none() {
                record_routing(InputRoutingDecision::ConsumedByCompositor, None);
                self.host.post_to_presentation(
                    presentation_id,
                    compositor_presentation_client::messages::DidConsumeInputEvent {
                        context_id,
                        event_id: event.id,
                    },
                );
                return;
            }
            event.scrollbar_dragged_by_compositor = result.scrollbar_dragged_by_compositor;
        }
        record_routing(InputRoutingDecision::ForwardedToPage, None);
        context
            .borrow()
            .dispatch_mouse_event_to_web_content(&event, self.nested_context_scrolled_since_last_frame(context_id));
    }
}

// The content box of a nested context is drawn scaled to the size of its viewport.
pub(super) fn position_in_nested_context(
    hit: &NestedContextHit,
    nested_context: &RefCell<ContextState>,
) -> Option<FloatPoint> {
    let viewport_size = nested_context.borrow().viewport_size();
    if hit.content_box_size.is_empty() || viewport_size.width <= 0 || viewport_size.height <= 0 {
        return None;
    }
    Some(FloatPoint {
        x: hit.position_in_content_box.x * viewport_size.width as f32 / hit.content_box_size.width,
        y: hit.position_in_content_box.y * viewport_size.height as f32 / hit.content_box_size.height,
    })
}

impl CompositorCore {
    // Where the position in the presenting context lands in the nested context, through each context in between and
    // without regard to where their content is or what clips it.
    pub(super) fn position_in_nested_context_ignoring_clips(
        &self,
        presenting_context_id: CompositorContextId,
        presenting_context: &RefCell<ContextState>,
        nested_context: &RefCell<ContextState>,
        mut position: FloatPoint,
    ) -> Option<FloatPoint> {
        let mut contexts_from_nested_context_outward = Vec::new();
        let mut context = nested_context;
        // A walk up the parents that outlasts the number of contexts went around a loop.
        let mut remaining_contexts = self.contexts.len();
        while !std::ptr::eq(context, presenting_context) {
            if remaining_contexts == 0 {
                return None;
            }
            contexts_from_nested_context_outward.push(context);
            let parent_context_id = context.borrow().parent_context_id()?;
            context = if parent_context_id == presenting_context_id {
                presenting_context
            } else {
                self.context_if_present(parent_context_id)?
            };
            remaining_contexts -= 1;
        }

        let mut containing_context = presenting_context;
        for context_in_between in contexts_from_nested_context_outward.into_iter().rev() {
            let context_in_between_id = context_in_between.borrow().context_id();
            let hit = containing_context
                .borrow_mut()
                .position_in_content_of_nested_context(context_in_between_id, position)?;
            let position_in_context = position_in_nested_context(&hit, context_in_between)?;
            position = position_in_context;
            containing_context = context_in_between;
        }
        Some(position)
    }

    // The scrollbars of a context nested in the presenting context `context_id` take the pointer over that context's
    // content, and a drag of one of them takes the pointer wherever it goes until the drag ends.
    fn handle_mouse_event_in_nested_contexts(
        &self,
        context_id: CompositorContextId,
        context: &RefCell<ContextState>,
        event: &MouseEvent,
        now: MonotonicTime,
    ) -> ContextUpdateResult {
        let mut pointer = context.borrow().pointer_in_nested_contexts_copy();
        let result = (|| {
            let mut context_with_captured_scrollbar =
                self.context_still_nested_in(context_id, pointer.context_with_captured_scrollbar);
            if context_with_captured_scrollbar.is_none_or(|context| !context.borrow().has_captured_scrollbar()) {
                pointer.context_with_captured_scrollbar = None;
                context_with_captured_scrollbar = None;
            }

            // The nested context presents and publishes what the event changed, and this context reports how the event went.
            let result_of_nested_context = |nested_context_id,
                                            nested_context: &RefCell<ContextState>,
                                            nested_result: &ContextUpdateResult,
                                            frame_of_this_context| {
                self.apply_context_update_result(nested_context_id, nested_context, nested_result);
                nested_context.borrow_mut().request_display_ticks_if_needed(now);
                // Node ids are only unique within the process that made them.
                let client = context.borrow().web_content_client_id();
                let ids_are_of_this_process = nested_context.borrow().is_owned_by(client);
                ContextUpdateResult {
                    accepted: nested_result.accepted,
                    frame_to_present: frame_of_this_context,
                    scrollbar_dragged_by_compositor: nested_result.scrollbar_dragged_by_compositor,
                    routing_decision: nested_result.routing_decision,
                    routed_scroller: if ids_are_of_this_process {
                        nested_result.routed_scroller
                    } else {
                        None
                    },
                    nested_context_hand_off: Some(InputRoutingDecision::HandedToNestedContext),
                    ..Default::default()
                }
            };

            let send_to_context_with_captured_scrollbar =
                |event_to_send: &MouseEvent,
                 pointer: &mut PointerInNestedContexts,
                 captured_context: &mut Option<&RefCell<ContextState>>| {
                    let nested_context_id = pointer.context_with_captured_scrollbar.unwrap();
                    let position = FloatPoint {
                        x: event_to_send.position.x as f32,
                        y: event_to_send.position.y as f32,
                    };
                    // A leave or a cancel has no position of its own.
                    if matches!(
                        event_to_send.r#type,
                        MouseEventType::MouseDown | MouseEventType::MouseMove | MouseEventType::MouseUp
                    ) {
                        let position_in_nested_context = self.position_in_nested_context_ignoring_clips(
                            context_id,
                            context,
                            captured_context.unwrap(),
                            position,
                        );
                        if let Some(position_in_nested_context) = position_in_nested_context {
                            pointer.last_position_in_context_with_captured_scrollbar = position_in_nested_context;
                        }
                    }
                    let nested_context = captured_context.unwrap();
                    let nested_result = nested_context.borrow_mut().handle_mouse_event_from_containing_context(
                        event_to_send,
                        pointer.last_position_in_context_with_captured_scrollbar,
                        now,
                    );
                    let result = result_of_nested_context(nested_context_id, nested_context, &nested_result, None);
                    if !nested_context.borrow().has_captured_scrollbar() {
                        pointer.context_with_captured_scrollbar = None;
                        *captured_context = None;
                        pointer.context_under_pointer = Some(nested_context_id);
                    }
                    result
                };

            // The scrollbar the pointer was over in the nested context it left is no longer hovered.
            let note_context_under_pointer = |nested_context_id, pointer: &mut PointerInNestedContexts| {
                if pointer.context_under_pointer == nested_context_id {
                    return;
                }
                if let Some(context_left) = self.context_still_nested_in(context_id, pointer.context_under_pointer) {
                    context_left.borrow_mut().note_pointer_position(None);
                    let frame_to_present = context_left.borrow_mut().clear_scrollbar_hover();
                    if let Some(frame_to_present) = frame_to_present {
                        self.request_present(pointer.context_under_pointer.unwrap(), &frame_to_present);
                    }
                }
                pointer.context_under_pointer = nested_context_id;
            };

            match event.r#type {
                MouseEventType::MouseDown | MouseEventType::MouseMove => {
                    if context_with_captured_scrollbar.is_some() {
                        if event.r#type == MouseEventType::MouseMove {
                            return send_to_context_with_captured_scrollbar(
                                event,
                                &mut pointer,
                                &mut context_with_captured_scrollbar,
                            );
                        }
                        // A press while a drag is still held means the release of that drag never came, so the drag ends there.
                        let mut cancel_event = event.clone();
                        cancel_event.r#type = MouseEventType::MouseCancel;
                        send_to_context_with_captured_scrollbar(
                            &cancel_event,
                            &mut pointer,
                            &mut context_with_captured_scrollbar,
                        );
                    }

                    let mut result = context.borrow_mut().handle_mouse_event(event, now);
                    if result.nested_context_mouse_hit.is_none() {
                        note_context_under_pointer(None, &mut pointer);
                        return result;
                    }

                    let frame_of_this_context = result.frame_to_present;
                    let mut containing_context_id = context_id;
                    let mut containing_context = context;
                    let mut position = FloatPoint::default();
                    while let Some(hit) = result.nested_context_mouse_hit.as_ref() {
                        // WebContent names the contexts its display lists draw, so a context only reaches those nested in it.
                        let nested_context = self.context_if_present(hit.child_context_id);
                        let mut position_in_nested_context_content = None;
                        if let Some(nested_context) = nested_context
                            && nested_context.borrow().parent_context_id() == Some(containing_context_id)
                        {
                            position_in_nested_context_content = position_in_nested_context(hit, nested_context);
                        }
                        let Some(position_in_nested_context_content) = position_in_nested_context_content else {
                            note_context_under_pointer(None, &mut pointer);
                            let mut declined =
                                ContextUpdateResult::declined(InputRoutingDecision::NestedContextUnavailable);
                            declined.frame_to_present = frame_of_this_context;
                            declined.nested_context_hand_off = Some(InputRoutingDecision::HandedToNestedContext);
                            return declined;
                        };
                        if !std::ptr::eq(containing_context, context) {
                            self.apply_context_update_result(containing_context_id, containing_context, &result);
                            containing_context.borrow_mut().request_display_ticks_if_needed(now);
                        }
                        containing_context_id = hit.child_context_id;
                        containing_context = nested_context.unwrap();
                        position = position_in_nested_context_content;
                        result = containing_context
                            .borrow_mut()
                            .handle_mouse_event_from_containing_context(event, position, now);
                    }

                    note_context_under_pointer(Some(containing_context_id), &mut pointer);
                    if event.r#type == MouseEventType::MouseDown && containing_context.borrow().has_captured_scrollbar()
                    {
                        pointer.context_with_captured_scrollbar = Some(containing_context_id);
                        pointer.last_position_in_context_with_captured_scrollbar = position;
                    }
                    result_of_nested_context(
                        containing_context_id,
                        containing_context,
                        &result,
                        frame_of_this_context,
                    )
                }
                MouseEventType::MouseUp => {
                    if context_with_captured_scrollbar.is_some() {
                        return send_to_context_with_captured_scrollbar(
                            event,
                            &mut pointer,
                            &mut context_with_captured_scrollbar,
                        );
                    }
                    context.borrow_mut().handle_mouse_event(event, now)
                }
                MouseEventType::MouseLeave | MouseEventType::MouseCancel => {
                    // The pointer leaves every nested context it was in, and a cancel ends the drag of one, as in this context.
                    let mut result_of_context_with_captured_scrollbar = None;
                    let mut context_with_captured_scrollbar_id = None;
                    if context_with_captured_scrollbar.is_some() {
                        context_with_captured_scrollbar_id = pointer.context_with_captured_scrollbar;
                        result_of_context_with_captured_scrollbar = Some(send_to_context_with_captured_scrollbar(
                            event,
                            &mut pointer,
                            &mut context_with_captured_scrollbar,
                        ));
                    }
                    if let Some(context_under_pointer) =
                        self.context_still_nested_in(context_id, pointer.context_under_pointer)
                        && pointer.context_under_pointer != context_with_captured_scrollbar_id
                    {
                        let nested_result = context_under_pointer
                            .borrow_mut()
                            .handle_mouse_event_from_containing_context(event, FloatPoint::default(), now);
                        self.apply_context_update_result(
                            pointer.context_under_pointer.unwrap(),
                            context_under_pointer,
                            &nested_result,
                        );
                        context_under_pointer.borrow_mut().request_display_ticks_if_needed(now);
                    }
                    pointer.context_under_pointer = None;

                    let result = context.borrow_mut().handle_mouse_event(event, now);
                    let Some(mut result_of_context_with_captured_scrollbar) = result_of_context_with_captured_scrollbar
                    else {
                        return result;
                    };
                    result_of_context_with_captured_scrollbar.frame_to_present = result.frame_to_present;
                    result_of_context_with_captured_scrollbar
                }
                MouseEventType::MouseWheel => context.borrow_mut().handle_mouse_event(event, now),
            }
        })();
        context.borrow_mut().set_pointer_in_nested_contexts(pointer);
        result
    }

    // A pinch is a ctrl+wheel event first, to the document under it, whose blocking wheel event listeners may cancel it
    // before anything zooms. Only the documents at the position can see that event.
    fn page_under_pinch_may_cancel_it(
        &self,
        context_id: CompositorContextId,
        context: &RefCell<ContextState>,
        mut position: FloatPoint,
    ) -> bool {
        let mut containing_context_id = context_id;
        let mut containing_context = context;
        // A walk down the nested contexts that outlasts the number of contexts went around a loop.
        for _ in 0..self.contexts.len() {
            if containing_context.borrow_mut().page_may_cancel_pinch_at(position) {
                return true;
            }
            let hit = containing_context.borrow_mut().nested_context_for_wheel_at(position);
            let Some(hit) = hit else { return false };
            // NB: Nothing is known yet of the listeners of a context that is not available.
            let Some(nested_context) = self.context_if_present(hit.child_context_id) else {
                return true;
            };
            if nested_context.borrow().parent_context_id() != Some(containing_context_id) {
                return true;
            }
            let Some(position_in_nested) = position_in_nested_context(&hit, nested_context) else {
                return true;
            };
            containing_context_id = hit.child_context_id;
            containing_context = nested_context;
            position = position_in_nested;
        }
        true
    }

    pub fn handle_and_dispatch_pinch_event(&self, context_id: CompositorContextId, mut event: PinchEvent) {
        let _entered = self.enter();

        // NB: The presentation connection only passes on input for the contexts bound to it.
        let context = self.context_if_present(context_id).expect("context exists");
        assert!(context.borrow().bound_presentation().is_some());
        let presentation_id = context.borrow().bound_presentation().unwrap();
        if !context.borrow().can_dispatch_input_to_web_content() {
            self.host.post_to_presentation(
                presentation_id,
                compositor_presentation_client::messages::DidNotDispatchInputEvent {
                    context_id,
                    event_id: event.id,
                },
            );
            return;
        }

        // The first step of a gesture decides for the steps after it: a gesture the page let through zooms from here on,
        // whatever listeners have appeared since, and any other leaves each step to the page, wherever the pinch has moved.
        // A page that already let this step through leaves only the zoom to do.
        let position = FloatPoint {
            x: event.position.x as f32,
            y: event.position.y as f32,
        };
        let may_zoom = if event.continues_gesture {
            context.borrow().pinch_gesture_was_let_through()
        } else {
            !self.page_under_pinch_may_cancel_it(context_id, context, position)
        };
        if event.wheel_event_was_dispatched || may_zoom {
            let result = context.borrow_mut().handle_pinch_event(&event);
            event.async_zoom_performed_default_action = self.apply_context_update_result(context_id, context, &result);
        }
        context
            .borrow_mut()
            .set_pinch_gesture_was_let_through(event.async_zoom_performed_default_action);

        // The scroll updates the event produced reach WebContent ahead of the event itself on the same connection.
        self.publish_pending_async_scroll_updates(context_id, context);
        context.borrow().dispatch_pinch_event_to_web_content(&event);
        self.host.post_to_presentation(
            presentation_id,
            compositor_presentation_client::messages::DidDispatchInputEvent {
                context_id,
                event_id: event.id,
                performed_default_action: event.async_zoom_performed_default_action,
            },
        );
    }
}
