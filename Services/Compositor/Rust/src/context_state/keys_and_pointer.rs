/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Keyboard and pointer input for a compositor context.

use super::*;

impl ContextState {
    fn key_scroll_for(&self, event: &KeyEvent) -> Option<KeyScroll> {
        if !self.presents_to_client()
            || self.paused_debugger_overlay_is_shown
            || self.visibility != ContextVisibility::Visible
            || !is_keyboard_scroll_key(event.key, event.modifiers.0)
            || !self.keyboard_scroll_controller.has_target()
            || self.scene.visual_context_tree().is_none()
            || !self
                .keyboard_scroll_controller
                .state_matches_visual_context_tree_epoch(self.scene.current_visual_context_tree().structural_epoch())
        {
            return None;
        }
        let key_step = self
            .keyboard_scroll_controller
            .step_for_key(event, &self.async_scroll_tree)?;
        let mut target_scroll_node_index = self.async_scroll_tree.scroll_node_for_keyboard_scroll(
            key_step.keyboard_scroll_target,
            key_step.delta,
            &self.scroll_state_snapshot_at_keyboard_step_starts(),
        );
        // If every destination is at its boundary, consume repeats while a scrolling box can still move toward it.
        // Falling back would cancel the running animation and lose the steps already accumulated in its destination.
        if target_scroll_node_index.is_none() {
            target_scroll_node_index = self.async_scroll_tree.scroll_node_for_keyboard_scroll(
                key_step.keyboard_scroll_target,
                key_step.delta,
                &self.scroll_state_snapshot,
            );
        }
        Some(KeyScroll {
            step: key_step,
            target_scroll_node_index,
        })
    }

    pub fn box_scrolled_for_key_event(&self, event: &KeyEvent) -> Option<AsyncScrollNodeStableID> {
        if event.r#type != KeyEventType::KeyDown {
            return None;
        }
        let key_scroll = self.key_scroll_for(event)?;
        Some(
            self.async_scroll_tree
                .scroll_node_for_index(key_scroll.target_scroll_node_index?)?
                .stable_node_id,
        )
    }

    pub fn handle_key_event(&mut self, event: &KeyEvent, now: MonotonicTime) -> ContextUpdateResult {
        if !is_keyboard_scroll_key(event.key, KeyModifier::Mod_None.0) {
            return Default::default();
        }
        // Release the key even if focus, modifiers, or routing eligibility changed while it was held. Other scroll
        // keys may still be held, so only end the gesture when the last one is released.
        if event.r#type == KeyEventType::KeyUp {
            let release = self.keyboard_scroll_controller.release_key(event);
            if release == KeyRelease::Ignored {
                return Default::default();
            }
            if release == KeyRelease::ReleasedLastHeldKey {
                self.end_user_scroll_gesture_of_held_keys();
            }
            return ContextUpdateResult {
                accepted: true,
                should_request_rendering_update: true,
                ..Default::default()
            };
        }
        let Some(key_scroll) = self.key_scroll_for(event) else {
            return Default::default();
        };
        let key_step = &key_scroll.step;
        let mut delta = key_step.delta;
        // https://drafts.csswg.org/cssom-view/#viewport-perform-a-scroll
        // The visual viewport takes as much of the scroll as it can before the layout viewport takes the rest.
        let mut panned_visual_viewport = false;
        if key_scroll
            .target_scroll_node_index
            .is_none_or(|index| self.async_scroll_tree.scroll_node_is_viewport(index))
        {
            let magnification = self.scene.visual_viewport_magnification();
            let delta_in_screen_pixels = delta.scaled(magnification);
            let panned_delta = self.pan_visual_viewport_by(delta_in_screen_pixels);
            panned_visual_viewport = !panned_delta.is_zero();
            delta = (delta_in_screen_pixels - panned_delta).scaled(1.0 / magnification);
        }
        let result_of_pan = |viewport_rect| ContextUpdateResult {
            accepted: true,
            frame_to_present: Some(PendingFrame::repainting_changes(viewport_rect)),
            should_request_rendering_update: true,
            ..Default::default()
        };
        // NB: With nothing to scroll here, the main thread must not hold back the keys after this one. It still gets to
        //     scroll for this one, as content it added may have extended the scroll range since the compositor last heard.
        let Some(target_scroll_node_index) = key_scroll.target_scroll_node_index else {
            if panned_visual_viewport {
                return result_of_pan(self.async_scrolling_viewport_rect);
            }
            return ContextUpdateResult {
                accepted: true,
                key_found_nothing_to_scroll: true,
                ..Default::default()
            };
        };
        if delta.is_zero() {
            return result_of_pan(self.async_scrolling_viewport_rect);
        }
        let scrolls_to_extent = key_step.scrolls_to_extent;
        self.keyboard_scroll_controller.hold_key(event.key);
        let Some(node) = self.async_scroll_tree.scroll_node_for_index(target_scroll_node_index) else {
            return Default::default();
        };
        let stable_node_id = node.stable_node_id;
        if !self.boxes_scrolled_by_held_keys.contains(&stable_node_id) {
            self.boxes_scrolled_by_held_keys.push(stable_node_id);
        }
        let mut scroll_in_flight_destination = None;
        for running_animation in &self.smooth_scroll_animations {
            if running_animation.stable_node_id == stable_node_id
                && running_animation.is_user_scroll
                && running_animation.animation.kind() == ScrollAnimationKind::SmoothScroll
            {
                scroll_in_flight_destination = Some(running_animation.animation.destination_offset());
            }
        }
        let css_scroll_in_flight_destination =
            scroll_in_flight_destination.map(|offset| self.async_scroll_tree.css_pixels_from_device_offset(offset));
        if let Some(decision) = self.scroll_snap_controller.decide_key_step(
            &self.async_scroll_tree,
            &self.scroll_state_snapshot,
            target_scroll_node_index,
            self.async_scroll_tree.css_pixels_from_device_offset(delta),
            key_step.snap_selection_intent,
            css_scroll_in_flight_destination,
            now,
        ) {
            self.schedule_end_of_scroll_step_gestures(now);
            match decision {
                StepDecision::SnapScrollStart(mut snap_scroll) => {
                    snap_scroll.is_absolute_scroll = scrolls_to_extent;
                    self.start_snap_scroll(target_scroll_node_index, snap_scroll, false, now);
                    return result_of_pan(self.async_scrolling_viewport_rect);
                }
                StepDecision::StepConsumed(consumed) => {
                    if let Some(mut updated_scroll) = consumed.updated_scroll {
                        updated_scroll.is_absolute_scroll = scrolls_to_extent;
                        self.note_user_input_scrolled(updated_scroll.stable_node_id);
                        self.async_scroll_update_outbox
                            .record_started_user_scroll(updated_scroll);
                    }
                }
            }
            return ContextUpdateResult {
                accepted: true,
                should_request_rendering_update: true,
                ..Default::default()
            };
        }
        let current_offset = self
            .async_scroll_tree
            .scroll_offset_for_node(target_scroll_node_index, &self.scroll_state_snapshot);
        assert!(current_offset.is_some());
        // A plain step continues toward the animation's destination, including any snap on the other axis.
        let step_start = scroll_in_flight_destination.unwrap_or(current_offset.unwrap());
        let destination = self.async_scroll_tree.clamped_scroll_offset_for_node(
            target_scroll_node_index,
            FloatPoint {
                x: step_start.x + delta.x,
                y: step_start.y + delta.y,
            },
        );
        if destination == step_start {
            if panned_visual_viewport {
                return result_of_pan(self.async_scrolling_viewport_rect);
            }
            return ContextUpdateResult {
                accepted: true,
                should_request_rendering_update: true,
                ..Default::default()
            };
        }
        for index in 0..self.smooth_scroll_animations.len() {
            let animation = &self.smooth_scroll_animations[index];
            if animation.stable_node_id != stable_node_id
                || !animation.is_user_scroll
                || animation.animation.kind() != ScrollAnimationKind::SmoothScroll
            {
                continue;
            }
            self.retarget_user_scroll(index, destination, now);
            self.smooth_scroll_animations[index].heads_to_an_extent = scrolls_to_extent;
            let operation_id = self.smooth_scroll_animations[index].operation_id;
            self.report_started_user_scroll(
                operation_id,
                stable_node_id,
                step_start,
                destination,
                ScrollAnimationKind::SmoothScroll,
                scrolls_to_extent,
            );
            return result_of_pan(self.async_scrolling_viewport_rect);
        }
        self.cancel_smooth_scroll_taken_over_by_user_input(target_scroll_node_index);
        let operation_id = self.async_scroll_update_outbox.allocate_operation_id();
        self.smooth_scroll_animations.push(ActiveSmoothScrollAnimation {
            stable_node_id,
            operation_id,
            animation: SmoothScrollAnimation::new(
                current_offset.unwrap(),
                destination,
                self.async_scroll_tree.device_pixels_per_css_pixel(),
                ScrollAnimationKind::SmoothScroll,
            ),
            started_at: now,
            is_user_scroll: true,
            heads_to_an_extent: scrolls_to_extent,
        });
        self.report_started_user_scroll(
            operation_id,
            stable_node_id,
            step_start,
            destination,
            ScrollAnimationKind::SmoothScroll,
            scrolls_to_extent,
        );
        result_of_pan(self.async_scrolling_viewport_rect)
    }

    fn scroll_state_snapshot_at_keyboard_step_starts(&self) -> ScrollStateSnapshot {
        let mut snapshot = self.scroll_state_snapshot.clone();
        for running_animation in &self.smooth_scroll_animations {
            // A key continues a keyboard animation, but takes over other animations from the presented offset.
            if !running_animation.is_user_scroll
                || running_animation.animation.kind() != ScrollAnimationKind::SmoothScroll
            {
                continue;
            }
            let Some(scroll_node_index) = self
                .async_scroll_tree
                .scroll_node_index_for_stable_id(running_animation.stable_node_id)
            else {
                continue;
            };
            let mut destination = running_animation.animation.destination_offset();
            // A snap destination at the boundary can still consume steps until their accumulated input reaches it.
            if let Some(unsnapped_destination) = self
                .scroll_snap_controller
                .unsnapped_destination_for_snap_scroll(running_animation.stable_node_id, running_animation.operation_id)
            {
                destination = self
                    .async_scroll_tree
                    .device_offset_from_css_pixels(unsnapped_destination);
            }
            snapshot.set_device_offset_for_index(
                scroll_node_index,
                FloatPoint {
                    x: -destination.x,
                    y: -destination.y,
                },
            );
        }
        snapshot
    }

    pub fn handle_mouse_event(&mut self, event: &MouseEvent, now: MonotonicTime) -> ContextUpdateResult {
        if !self.presents_to_client() {
            return ContextUpdateResult::declined(InputRoutingDecision::ContextNotPresenting);
        }
        let position = FloatPoint {
            x: event.position.x as f32,
            y: event.position.y as f32,
        };
        self.handle_mouse_event_at(event, position, now)
    }

    // A mouse event that the context this one is nested in hands to it, at a position in this context's device pixels.
    pub fn handle_mouse_event_from_containing_context(
        &mut self,
        event: &MouseEvent,
        position: FloatPoint,
        now: MonotonicTime,
    ) -> ContextUpdateResult {
        if event.r#type == MouseEventType::MouseMove {
            self.note_pointer_position(Some(position));
        } else if matches!(event.r#type, MouseEventType::MouseLeave | MouseEventType::MouseCancel) {
            self.note_pointer_position(None);
        }
        self.handle_mouse_event_at(event, position, now)
    }

    // Where the pointer is in this context's device pixels, as the mouse event the context takes or hands on says, or
    // nowhere, as the pointer leaves it.
    pub fn note_pointer_position(&mut self, position: Option<FloatPoint>) {
        self.pointer_position = position;
    }

    // The compositor presents how scrollbars expand, outside of what damage tracking sees.
    fn frame_repainting_scrollbars(&self) -> Option<PendingFrame> {
        if self.async_scrolling_viewport_rect.is_empty() {
            return None;
        }
        Some(PendingFrame::repainting_everything(self.async_scrolling_viewport_rect))
    }

    // The pointer left the scrollbar it was over here for a context nested in this one, or for the one around it.
    pub fn clear_scrollbar_hover(&mut self) -> Option<PendingFrame> {
        if !self.scrollbar_controller.set_hovered_scrollbar(None) {
            return None;
        }
        self.frame_repainting_scrollbars()
    }

    fn scrollbar_under_pointer(&mut self, position: FloatPoint) -> Option<usize> {
        if let Some(index) = self.scrollbar_controller.hit_test_scrollbar_painted_by_compositor(
            &self.async_scroll_tree,
            &self.scroll_state_snapshot,
            position,
        ) {
            return Some(index);
        }
        self.scrollbar_controller.hit_test_scrollbar_painted_by_display_list(
            &self.async_scroll_tree,
            self.scene.visual_context_tree_for_compositing(),
            &self.scroll_state_snapshot,
            position,
        )
    }

    // The compositor paints its own scrollbars over everything, so a nested context only takes the pointer where none of
    // them is under it, and where no scrollbar the display list paints comes after the nested context's content in paint
    // order. A press passes by a scrollbar that has faded out, as it does here.
    fn nested_context_taking_mouse_event_at(
        &mut self,
        event_type: MouseEventType,
        position: FloatPoint,
    ) -> Option<NestedContextHit> {
        if !self.has_async_scrolling_state {
            return None;
        }
        let tree = self.scene.visual_context_tree_for_compositing();
        let hit = self.async_scroll_tree.nested_context_at(tree, position)?;
        if let Some(index) = self.scrollbar_controller.hit_test_scrollbar_painted_by_compositor(
            &self.async_scroll_tree,
            &self.scroll_state_snapshot,
            position,
        ) && (event_type != MouseEventType::MouseDown || self.scrollbar_controller.is_shown(index))
        {
            return None;
        }
        if let Some(index) = self.scrollbar_controller.hit_test_scrollbar_painted_by_display_list(
            &self.async_scroll_tree,
            tree,
            &self.scroll_state_snapshot,
            position,
        ) && self
            .scrollbar_controller
            .scrollbars()
            .get(index)
            .is_some_and(|scrollbar| scrollbar.paint_order_index > hit.paint_order_index)
        {
            return None;
        }
        Some(hit)
    }

    pub fn position_in_content_of_nested_context(
        &mut self,
        nested_context_id: CompositorContextId,
        position: FloatPoint,
    ) -> Option<NestedContextHit> {
        if !self.has_async_scrolling_state {
            return None;
        }
        self.async_scroll_tree.position_in_content_of_nested_context(
            nested_context_id,
            self.scene.visual_context_tree_for_compositing(),
            position,
        )
    }

    #[allow(clippy::field_reassign_with_default)]
    fn handle_mouse_event_at(
        &mut self,
        event: &MouseEvent,
        position: FloatPoint,
        now: MonotonicTime,
    ) -> ContextUpdateResult {
        if self.scrollbar_controller.is_empty() && !self.async_scroll_tree.has_nested_contexts() {
            return ContextUpdateResult::declined(InputRoutingDecision::NotOverAScrollbar);
        }
        let scroller_of_scrollbar = |controller: &ScrollbarController, index: usize| {
            controller
                .scrollbars()
                .get(index)
                .and_then(|scrollbar| scrollbar.scroller_stable_node_id)
        };
        // The pointer goes on into a nested context, and leaves the scrollbar here it was over.
        let hand_to_nested_context = |state: &mut Self, hit| ContextUpdateResult {
            frame_to_present: state.clear_scrollbar_hover(),
            nested_context_mouse_hit: Some(hit),
            ..Default::default()
        };
        match event.r#type {
            MouseEventType::MouseDown => {
                if event.button != MouseButton::Primary {
                    return ContextUpdateResult::declined(InputRoutingDecision::NotPrimaryButton);
                }
                if let Some(hit) = self.nested_context_taking_mouse_event_at(event.r#type, position) {
                    return hand_to_nested_context(self, hit);
                }
                let Some(drag) = self.scrollbar_controller.begin_drag(
                    &self.async_scroll_tree,
                    self.scene.visual_context_tree_for_compositing(),
                    &self.scroll_state_snapshot,
                    position,
                ) else {
                    return ContextUpdateResult::declined(InputRoutingDecision::NotOverAScrollbar);
                };
                self.scrollbar_press_scroll_operation = None;
                self.stop_scrollbar_paging();
                let mut result = ContextUpdateResult {
                    accepted: true,
                    ..Default::default()
                };
                result.routing_decision = Some(InputRoutingDecision::BeganScrollbarDrag);
                result.routed_scroller = scroller_of_scrollbar(&self.scrollbar_controller, drag.scrollbar_index);
                result.scrollbar_dragged_by_compositor = self
                    .scrollbar_controller
                    .captured_scrollbar_painted_by_display_list(self.context_id);
                result.frame_to_present = self.frame_repainting_scrollbars();
                // A press beside the thumb pages toward the pointer or jumps there, as the platform says, and a modifier swaps
                // the two.
                let press_jumps = self.scrollbar_track_press_jumps
                    != !(event.modifiers & SCROLLBAR_TRACK_PRESS_INVERTING_MODIFIER).is_empty();
                if drag.is_press_beside_thumb && !press_jumps {
                    let Some(scrollbar) = self.scrollbar_controller.scrollbars().get(drag.scrollbar_index) else {
                        return result;
                    };
                    let scroll_offset = self
                        .async_scroll_tree
                        .scroll_offset_for_node(scrollbar.scroll_node_index, &self.scroll_state_snapshot);
                    assert!(scroll_offset.is_some());
                    self.scrollbar_paging = Some(ScrollbarPaging {
                        direction: self.scrollbar_controller.paging_direction(
                            drag.scrollbar_index,
                            drag.primary_position,
                            scroll_offset.unwrap(),
                        ),
                        primary_position: drag.primary_position,
                    });
                    if let Some(frame) = self.page_captured_scrollbar(now) {
                        result.frame_to_present = Some(frame);
                        result.should_request_rendering_update = true;
                    }
                    self.host.set_timer(
                        self.context_id,
                        ContextTimer::ScrollbarPaging,
                        SCROLLBAR_PAGING_INITIAL_DELAY,
                    );
                    self.scrollbar_paging_timer_is_armed = true;
                    return result;
                }
                if let Some(frame) = self.apply_scrollbar_drag(&drag, now) {
                    result.frame_to_present = Some(frame);
                    result.should_request_rendering_update = true;
                }
                result
            }
            MouseEventType::MouseMove => {
                if self.scrollbar_controller.has_captured_scrollbar() {
                    let scrollbar_dragged_by_compositor = self
                        .scrollbar_controller
                        .captured_scrollbar_painted_by_display_list(self.context_id);
                    let drag = self.scrollbar_controller.captured_drag(
                        self.scene.visual_context_tree_for_compositing(),
                        &self.scroll_state_snapshot,
                        position,
                    );
                    assert!(drag.is_some());
                    let drag = drag.unwrap();
                    // While paging, the pointer only says where the thumb pages to.
                    let mut frame_to_present = None;
                    if let Some(paging) = &mut self.scrollbar_paging {
                        paging.primary_position = drag.primary_position;
                        if !self.scrollbar_paging_timer_is_armed {
                            self.host.set_timer(
                                self.context_id,
                                ContextTimer::ScrollbarPaging,
                                SCROLLBAR_PAGING_REPEAT_INTERVAL,
                            );
                            self.scrollbar_paging_timer_is_armed = true;
                        }
                    } else {
                        frame_to_present = self.apply_scrollbar_drag(&drag, now);
                    }
                    return ContextUpdateResult {
                        accepted: true,
                        frame_to_present,
                        should_request_rendering_update: frame_to_present.is_some(),
                        scrollbar_dragged_by_compositor,
                        routing_decision: Some(InputRoutingDecision::ContinuedScrollbarDrag),
                        routed_scroller: scroller_of_scrollbar(&self.scrollbar_controller, drag.scrollbar_index),
                        ..Default::default()
                    };
                }
                if let Some(hit) = self.nested_context_taking_mouse_event_at(event.r#type, position) {
                    return hand_to_nested_context(self, hit);
                }
                let hovered_scrollbar_index = self.scrollbar_under_pointer(position);
                let mut frame_to_present = None;
                if self.scrollbar_controller.set_hovered_scrollbar(hovered_scrollbar_index) {
                    frame_to_present = self.frame_repainting_scrollbars();
                }
                let Some(index) = hovered_scrollbar_index else {
                    return ContextUpdateResult {
                        frame_to_present,
                        routing_decision: Some(InputRoutingDecision::NotOverAScrollbar),
                        ..Default::default()
                    };
                };
                // The page sees the pointer move over a scrollbar the display list paints, as it is part of the page's content.
                let Some(scrollbar) = self.scrollbar_controller.scrollbars().get(index) else {
                    return ContextUpdateResult::default();
                };
                let is_painted_by_compositor = scrollbar.is_painted_by_compositor;
                ContextUpdateResult {
                    accepted: is_painted_by_compositor,
                    frame_to_present,
                    should_request_rendering_update: false,
                    routing_decision: Some(if is_painted_by_compositor {
                        InputRoutingDecision::HoveringScrollbarPaintedByCompositor
                    } else {
                        InputRoutingDecision::HoveringScrollbarPaintedByDisplayList
                    }),
                    routed_scroller: scroller_of_scrollbar(&self.scrollbar_controller, index),
                    ..Default::default()
                }
            }
            MouseEventType::MouseUp => {
                if event.button != MouseButton::Primary {
                    return ContextUpdateResult::declined(InputRoutingDecision::NotPrimaryButton);
                }
                let box_of_dragged_scrollbar = self.box_of_captured_scrollbar();
                let scrollbar_dragged_by_compositor = self
                    .scrollbar_controller
                    .captured_scrollbar_painted_by_display_list(self.context_id);
                let drag = self.scrollbar_controller.release_captured_drag(
                    self.scene.visual_context_tree_for_compositing(),
                    &self.scroll_state_snapshot,
                    position,
                );
                let was_paging = self.scrollbar_paging.is_some();
                self.stop_scrollbar_paging();
                let Some(drag) = drag else {
                    return ContextUpdateResult::declined(InputRoutingDecision::NoScrollbarDrag);
                };
                self.note_user_scroll_gesture_end_if_drag_ended(box_of_dragged_scrollbar);
                let mut result = ContextUpdateResult {
                    accepted: true,
                    ..Default::default()
                };
                result.routing_decision = Some(InputRoutingDecision::EndedScrollbarDrag);
                result.routed_scroller = scroller_of_scrollbar(&self.scrollbar_controller, drag.scrollbar_index);
                result.scrollbar_dragged_by_compositor = scrollbar_dragged_by_compositor;
                // The main thread learns of the release from the next update it takes, so one is asked for even when the
                // release scrolls nothing.
                result.should_request_rendering_update = true;
                result.frame_to_present = self.frame_repainting_scrollbars();
                if !was_paging && let Some(frame) = self.apply_scrollbar_drag(&drag, now) {
                    result.frame_to_present = Some(frame);
                }
                let hovered = self.scrollbar_under_pointer(position);
                if self.scrollbar_controller.set_hovered_scrollbar(hovered)
                    && let Some(frame) = self.frame_repainting_scrollbars()
                {
                    result.frame_to_present = Some(frame);
                }
                result
            }
            MouseEventType::MouseLeave => {
                let had_capture = self.scrollbar_controller.has_captured_scrollbar();
                let mut frame_to_present = None;
                if self.scrollbar_controller.set_hovered_scrollbar(None) {
                    frame_to_present = self.frame_repainting_scrollbars();
                }
                ContextUpdateResult {
                    accepted: had_capture,
                    frame_to_present,
                    should_request_rendering_update: false,
                    scrollbar_dragged_by_compositor: self
                        .scrollbar_controller
                        .captured_scrollbar_painted_by_display_list(self.context_id),
                    routing_decision: Some(if had_capture {
                        InputRoutingDecision::LeftDuringScrollbarDrag
                    } else {
                        InputRoutingDecision::NoScrollbarDrag
                    }),
                    ..Default::default()
                }
            }
            MouseEventType::MouseCancel => {
                // A cancel ends a drag where it last scrolled to, as a cancel has no position of its own, and the pointer then
                // counts as having left. The page still sees the cancel, so it is not accepted.
                let was_dragging_scrollbar = self.scrollbar_controller.has_captured_scrollbar();
                let box_of_dragged_scrollbar = self.box_of_captured_scrollbar();
                self.scrollbar_controller.release_captured_drag(
                    self.scene.visual_context_tree_for_compositing(),
                    &self.scroll_state_snapshot,
                    position,
                );
                self.stop_scrollbar_paging();
                self.note_user_scroll_gesture_end_if_drag_ended(box_of_dragged_scrollbar);
                let hovered_scrollbar_changed = self.scrollbar_controller.set_hovered_scrollbar(None);
                let mut result = ContextUpdateResult::default();
                // As for a release, the main thread learns that the drag ended from the next update it takes.
                result.should_request_rendering_update = was_dragging_scrollbar;
                if was_dragging_scrollbar || hovered_scrollbar_changed {
                    result.frame_to_present = self.frame_repainting_scrollbars();
                }
                result
            }
            MouseEventType::MouseWheel => ContextUpdateResult::default(),
        }
    }
}
