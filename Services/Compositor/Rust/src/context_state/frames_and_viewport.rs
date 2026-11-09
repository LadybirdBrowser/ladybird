/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Frames, viewport state, and scroll update publication for a compositor context.

use super::*;
use std::fmt::Write;

struct JsonWriter(String);

impl JsonWriter {
    fn string(&mut self, value: &str) {
        self.0.push('"');
        for ch in value.chars() {
            match ch {
                '\u{8}' => self.0.push_str("\\b"),
                '\n' => self.0.push_str("\\n"),
                '\t' => self.0.push_str("\\t"),
                '"' => self.0.push_str("\\\""),
                '\\' => self.0.push_str("\\\\"),
                '\u{0}'..='\u{1f}' => {
                    write!(self.0, "\\u{:04x}", ch as u32).unwrap();
                }
                _ => self.0.push(ch),
            }
        }
        self.0.push('"');
    }

    fn key(&mut self, key: &str) {
        if !self.0.ends_with('{') {
            self.0.push(',');
        }
        self.string(key);
        self.0.push(':');
    }

    fn number(&mut self, value: f64) {
        if value.is_finite() {
            write!(self.0, "{value}").unwrap();
        } else {
            self.0.push_str("null");
        }
    }

    fn point(&mut self, x: f64, y: f64) {
        self.0.push_str("{\"x\":");
        self.number(x);
        self.key("y");
        self.number(y);
        self.0.push('}');
    }
}

impl ContextState {
    pub(super) fn wheel_step_can_stretch_elastically(
        &self,
        wheel_delta_precision: WheelDeltaPrecision,
        scroll_gesture_phase: ScrollGesturePhase,
    ) -> bool {
        self.presents_to_client()
            && self
                .elastic_overscroll_controller
                .step_can_stretch(wheel_delta_precision, scroll_gesture_phase)
    }

    pub(super) fn pull_elastic_overscroll_by(
        &mut self,
        node: &AsyncScrollNode,
        step_delta: FloatPoint,
        delta_the_scroller_did_not_scroll_by: FloatPoint,
    ) -> bool {
        let scroll_offset = self
            .async_scroll_tree
            .scroll_offset_for_node(node.scroll_node_index, &self.scroll_state_snapshot);
        let Some(scroll_offset) = scroll_offset else {
            return false;
        };
        self.elastic_overscroll_controller.pull_by(
            node,
            scroll_offset,
            step_delta,
            delta_the_scroller_did_not_scroll_by,
            self.scene.visual_viewport_magnification(),
        )
    }

    pub fn set_scroll_capabilities(&mut self, capabilities: &ScrollCapabilities) {
        self.presentation_synthesizes_scroll_momentum = capabilities.synthesizes_scroll_momentum;
        self.elastic_overscroll_controller
            .set_enabled(capabilities.scrolls_elastically);
        self.scrollbar_track_press_jumps = capabilities.scrollbar_track_press_jumps;
        self.wheel_gesture_routing
            .set_touchpad_gesture_end(capabilities.touchpad_gesture_end);
    }

    pub fn advance_elastic_overscroll_rebounds(&mut self, frame_time: MonotonicTime) -> Option<IntRect> {
        if !self.elastic_overscroll_controller.advance_rebounds(frame_time) {
            return None;
        }
        Some(self.async_scrolling_viewport_rect)
    }

    // Where the first step of a wheel gesture at the position, by the delta in CSS pixels, would go.
    pub fn route_first_wheel_step_for_testing(
        &mut self,
        position: FloatPoint,
        delta_in_css_pixels: FloatPoint,
        nested_context_regions: NestedContextRegions,
    ) -> FirstWheelStepRouteForTesting {
        let document_id = self.async_scroll_tree.document_id();
        if !self.presents_to_client() {
            return FirstWheelStepRouteForTesting {
                route: libcompositing_rust::scrolling::FirstWheelStepRoute {
                    decision: InputRoutingDecision::ContextNotPresenting,
                    scroller: None,
                },
                document_id,
            };
        }
        if self.scene.display_list().is_none() || self.scene.visual_context_tree().is_none() {
            return FirstWheelStepRouteForTesting {
                route: libcompositing_rust::scrolling::FirstWheelStepRoute {
                    decision: InputRoutingDecision::NoAsyncScrollingState,
                    scroller: None,
                },
                document_id,
            };
        }
        let delta = delta_in_css_pixels.scaled(
            self.async_scroll_tree.device_pixels_per_css_pixel() as f32 / self.scene.visual_viewport_magnification(),
        );
        FirstWheelStepRouteForTesting {
            route: self.async_scroll_tree.route_first_wheel_step(
                self.wheel_gesture_routing.admission(),
                self.scene.visual_context_tree_for_compositing(),
                position,
                delta,
                nested_context_regions,
            ),
            document_id,
        }
    }

    // The installed scrolling state, its routing admission, and how far the given scroller is stretched, as JSON.
    pub fn scroll_state_for_testing(&self, scroller: AsyncScrollNodeStableID) -> String {
        let mut scroll_state = JsonWriter(async_scrolling_state_to_json(self.async_scroll_tree.state()));
        scroll_state.0.pop();
        let admission = self.wheel_gesture_routing.admission();
        let admission_name = if admission == WheelRoutingAdmission::Accepted {
            "accepted".to_string()
        } else {
            input_routing_decision_for_wheel_step_not_admitted_by(admission).name()
        };
        scroll_state.key("wheelRoutingAdmission");
        scroll_state.string(&admission_name);
        if let Some(index) = self.async_scroll_tree.scroll_node_index_for_stable_id(scroller)
            && let Some(offset) = self
                .async_scroll_tree
                .css_scroll_offset_for_node(index, &self.scroll_state_snapshot)
        {
            scroll_state.key("scrollOffset");
            scroll_state.point(offset.x.to_double(), offset.y.to_double());
        }
        let stretch = self
            .elastic_overscroll_controller
            .stretch_of(scroller)
            .scaled(1.0 / self.async_scroll_tree.device_pixels_per_css_pixel() as f32);
        scroll_state.key("stretch");
        scroll_state.point(stretch.x as f64, stretch.y as f64);
        scroll_state.key("visualViewportScale");
        scroll_state.number(self.scene.visual_viewport_scale().unwrap_or(1.0) as f64);
        let visual_viewport_offset = visual_viewport_scroll_offset(&self.scene.visual_viewport_transform());
        scroll_state.key("visualViewportOffset");
        scroll_state.point(visual_viewport_offset.x as f64, visual_viewport_offset.y as f64);
        scroll_state.0.push('}');
        scroll_state.0
    }

    pub(super) fn scroll_state_snapshot_stretched_by_elastic_overscrolls(&self) -> Option<ScrollStateSnapshot> {
        self.elastic_overscroll_controller
            .stretched_scroll_state_snapshot(&self.scroll_state_snapshot, &self.async_scroll_tree)
    }

    pub fn take_pending_async_scroll_updates(&mut self) -> PendingAsyncScrollUpdates {
        let mut updates = self
            .async_scroll_update_outbox
            .take_updates(self.user_scroll_gesture_state());
        updates.document_id = self.async_scroll_tree.document_id();
        updates
    }

    pub fn has_pending_async_scroll_updates(&self) -> bool {
        self.async_scroll_update_outbox
            .has_updates(&self.user_scroll_gesture_state())
    }

    pub fn publish_pending_async_scroll_updates(&mut self) {
        if !self.has_pending_async_scroll_updates() {
            return;
        }
        let updates = self.take_pending_async_scroll_updates();
        self.host.post_to_web_content(
            self.web_content_client_id,
            compositor_web_content_client::messages::AsyncScrollUpdates {
                context_id: self.context_id,
                updates,
            },
        );
    }

    pub fn viewport_size_updated(&mut self, viewport_size: IntSize) {
        self.scene.forget_whether_animated_content_may_affect_viewport();
        if self.viewport_size != viewport_size {
            self.keyboard_scroll_controller.forget_target();
        }
        self.viewport_size = viewport_size;
    }

    pub fn set_paused_debugger_overlay_is_shown(&mut self, is_shown: bool) {
        if is_shown {
            self.end_keyboard_scroll_gesture();
        }
        self.paused_debugger_overlay_is_shown = is_shown;
    }

    pub fn set_visibility(&mut self, visibility: ContextVisibility) -> bool {
        if self.visibility == visibility {
            return false;
        }
        self.visibility = visibility;
        if visibility == ContextVisibility::Hidden {
            self.end_keyboard_scroll_gesture();
            self.elastic_overscroll_controller.forget();
        }
        true
    }

    // Where the compositor has scrolled each scroll node to.
    pub fn scroll_offsets(&self) -> Vec<CompositorScrollOffset> {
        let scroll_nodes = self.async_scroll_tree.scroll_nodes();
        let mut scroll_offsets = Vec::with_capacity(scroll_nodes.len());
        for node in scroll_nodes {
            scroll_offsets.push(CompositorScrollOffset {
                scroll_node: node.stable_node_id,
                offset: self
                    .async_scroll_tree
                    .css_scroll_offset_for_scroll_node(node, &self.scroll_state_snapshot),
            });
        }
        scroll_offsets
    }

    fn frame_inputs_as_presented_now(&mut self) -> FrameInputs {
        let stretched_scroll_state_snapshot = self.scroll_state_snapshot_stretched_by_elastic_overscrolls();
        let presented_scroll_state_snapshot =
            stretched_scroll_state_snapshot.unwrap_or_else(|| self.scroll_state_snapshot.clone());
        let scrollbars = self
            .scrollbar_controller
            .scrollbars_of_frame(&presented_scroll_state_snapshot);
        FrameInputs {
            visual_context_tree: self.scene.visual_context_tree_for_compositing().clone(),
            presented_scroll_state_snapshot,
            scrollbars,
            viewport_scroll_node_index: self.async_scroll_tree.viewport_scroll_node_index(),
        }
    }

    // Each frame shows the scrollbars as of when it is painted. An elastic overscroll moves the presented scroll offsets
    // only, and shows the scrollbars of the stretched scroller as a scroll does.
    fn update_scrollbar_visibility(&mut self, now: MonotonicTime) {
        let stretched_scroll_state_snapshot = self.scroll_state_snapshot_stretched_by_elastic_overscrolls();
        let presented_scroll_state_snapshot = stretched_scroll_state_snapshot
            .as_ref()
            .unwrap_or(&self.scroll_state_snapshot);
        self.scrollbar_controller
            .update_visibility(presented_scroll_state_snapshot, now);
        self.report_scrollbar_visibility_changes();
        // A hovered scrollbar starts to grow here, which needs display ticks before anything else asks for a frame.
        if self.scrollbar_controller.fades_need_frame(now) {
            self.host.request_display_ticks(self.context_id);
        }
        // The fade out runs on display ticks, which the wait before it does not need.
        let fade_out_time = self.scrollbar_controller.next_fade_out_time();
        let Some(fade_out_time) = fade_out_time else {
            self.host.cancel_timer(self.context_id, ContextTimer::ScrollbarFadeOut);
            return;
        };
        let delay = max(Duration::zero(), fade_out_time - now);
        self.host
            .set_timer(self.context_id, ContextTimer::ScrollbarFadeOut, delay);
    }

    // NB: This goes on the connection the compositor forwards input on, so the main thread knows which scrollbars have
    //     faded out before it hit tests any later press. A nested context another process owns reports to that process,
    //     which gets its input through the UI instead.
    fn report_scrollbar_visibility_changes(&mut self) {
        let changes = self.scrollbar_controller.take_visibility_changes();
        if !changes.is_empty() {
            self.host.post_to_web_content(
                self.web_content_client_id,
                compositor_web_content_client::messages::OverlayScrollbarVisibilitiesChanged { visibilities: changes },
            );
        }
    }

    pub fn advance_scrollbar_fades_and_return_damage_rect(&mut self, frame_time: MonotonicTime) -> IntRect {
        if self.scene.visual_context_tree().is_none() {
            return Default::default();
        }
        let stretched_scroll_state_snapshot = self.scroll_state_snapshot_stretched_by_elastic_overscrolls();
        let presented_scroll_state_snapshot = stretched_scroll_state_snapshot
            .as_ref()
            .unwrap_or(&self.scroll_state_snapshot);
        let damage_rect = self.scrollbar_controller.advance_fades(
            self.scene.visual_context_tree_for_compositing(),
            presented_scroll_state_snapshot,
            frame_time,
        );
        self.report_scrollbar_visibility_changes();
        damage_rect
    }

    // Each frame shows the scrollbars as of when it is painted, so this updates which of them are shown. Empty until a
    // display list is installed.
    pub fn frame_inputs(&mut self, now: MonotonicTime) -> Option<FrameInputs> {
        self.update_scrollbar_visibility(now);
        self.scene.display_list().as_ref()?;
        Some(self.frame_inputs_as_presented_now())
    }

    pub fn frame_inputs_for_screenshot(&mut self, now: MonotonicTime) -> ScreenshotInputs {
        assert!(self.scene.display_list().is_some());
        // Screenshot requests can arrive between display ticks, so sample animations and scrollbar visibility at capture
        // time.
        let capture_time = now;
        self.advance_visual_animations(capture_time);
        self.update_scrollbar_visibility(now);
        let scrollbar_fade_damage_rect = self.advance_scrollbar_fades_and_return_damage_rect(capture_time);
        ScreenshotInputs {
            frame_inputs: self.frame_inputs_as_presented_now(),
            scrollbar_fade_damage_rect,
        }
    }

    fn apply_visual_viewport_scroll_delta(&mut self, delta: FloatPoint) -> Option<VisualViewportScrollDelta> {
        if delta.is_zero() {
            return None;
        }
        self.scene.visual_context_tree().as_ref()?;
        let document_id = self.async_scroll_tree.document_id()?;
        let mut transform = self.scene.visual_viewport_transform();
        let scale = transform.matrix.elements[0][0];
        if scale <= 1.0 {
            return None;
        }
        let min_x = -(self.async_scrolling_viewport_rect.width as f32) * (scale - 1.0);
        let min_y = -(self.async_scrolling_viewport_rect.height as f32) * (scale - 1.0);
        let old_x = transform.matrix.elements[0][3];
        let old_y = transform.matrix.elements[1][3];
        let new_x = clamp(old_x - delta.x, min_x, 0.0);
        let new_y = clamp(old_y - delta.y, min_y, 0.0);
        let consumed_delta = FloatPoint {
            x: old_x - new_x,
            y: old_y - new_y,
        };
        if consumed_delta.is_zero() {
            return None;
        }
        transform.matrix.elements[0][3] = new_x;
        transform.matrix.elements[1][3] = new_y;
        self.scene.set_async_visual_viewport_transform(transform);
        self.visual_viewport_document_id = Some(document_id);
        self.note_user_input_scrolled(AsyncScrollNodeStableID {
            node_id: document_id,
            kind: AsyncScrollNodeKind::VisualViewport,
            pseudo_element_type: 0,
        });
        self.rebuild_wheel_hit_test_targets();
        Some(VisualViewportScrollDelta {
            scroll_offset: AsyncScrollOffset {
                stable_node_id: AsyncScrollNodeStableID {
                    node_id: document_id,
                    kind: AsyncScrollNodeKind::VisualViewport,
                    pseudo_element_type: 0,
                },
                compositor_scroll_offset: visual_viewport_scroll_offset(&transform),
                unadopted_scroll_delta: consumed_delta.scaled(1.0 / scale),
                last_relative_scroll_delta: Default::default(),
                visual_viewport_scale: Some(scale),
                applied_scroll_request_id: 0,
                user_input_scroll_count: 0,
                context_user_input_scroll_count: 0,
            },
            consumed_delta,
        })
    }

    // Pans the visual viewport of a zoomed page by as much of a delta in screen pixels as it has room for, and returns
    // that part of the delta.
    pub(super) fn pan_visual_viewport_by(&mut self, delta_in_screen_pixels: FloatPoint) -> FloatPoint {
        let Some(delta) = self.apply_visual_viewport_scroll_delta(delta_in_screen_pixels) else {
            return Default::default();
        };
        self.store_pending_async_scroll_offsets(&[delta.scroll_offset], None);
        delta.consumed_delta
    }

    pub(super) fn scroll_offsets_by_stable_id(&self) -> HashMap<AsyncScrollNodeStableID, FloatPoint> {
        let mut scroll_offsets = HashMap::new();
        if !self.has_async_scrolling_state {
            return scroll_offsets;
        }
        for node in self.async_scroll_tree.scroll_nodes() {
            scroll_offsets.insert(
                node.stable_node_id,
                AsyncScrollTree::scroll_offset_for_scroll_node(node, &self.scroll_state_snapshot),
            );
        }
        scroll_offsets
    }

    // The main thread asks for every scroll of its own, so the offsets a snapshot of its scroll state has for the boxes
    // this context already scrolls are what it last heard of them. Only a box new to this context takes its offset from
    // the snapshot.
    pub(super) fn keep_scroll_offsets_of_known_nodes(
        &mut self,
        scroll_offsets_of_known_nodes: &HashMap<AsyncScrollNodeStableID, FloatPoint>,
    ) -> Option<FloatPoint> {
        let mut viewport_scroll_offset = None;
        let mut clamped_scroll_offsets = Vec::new();
        let mut kept_any_scroll_offset = false;
        self.scroll_offsets_differ_from_last_frame = false;
        self.scroll_nodes_seeded_by_last_frame.clear();
        for node in self.async_scroll_tree.scroll_nodes() {
            let known_scroll_offset = scroll_offsets_of_known_nodes.get(&node.stable_node_id);
            let Some(&known_scroll_offset) = known_scroll_offset else {
                self.scroll_nodes_seeded_by_last_frame.insert(node.stable_node_id);
                continue;
            };
            if AsyncScrollTree::scroll_offset_for_scroll_node(node, &self.scroll_state_snapshot) != known_scroll_offset
            {
                self.scroll_offsets_differ_from_last_frame = true;
            }
            let kept_scroll_offset = self
                .async_scroll_tree
                .set_scroll_offset_leaving_sticky_offsets_unresolved(
                    node,
                    known_scroll_offset,
                    &mut self.scroll_state_snapshot,
                );
            kept_any_scroll_offset = true;
            if node.is_viewport {
                viewport_scroll_offset = Some(kept_scroll_offset);
            }
            // A box whose scroll range shrank past its offset is told where it ended up.
            if kept_scroll_offset != known_scroll_offset {
                clamped_scroll_offsets.push(AsyncScrollOffset {
                    stable_node_id: node.stable_node_id,
                    compositor_scroll_offset: kept_scroll_offset,
                    unadopted_scroll_delta: kept_scroll_offset - known_scroll_offset,
                    last_relative_scroll_delta: Default::default(),
                    visual_viewport_scale: None,
                    applied_scroll_request_id: 0,
                    user_input_scroll_count: 0,
                    context_user_input_scroll_count: 0,
                });
            }
        }
        // Resolving sticky offsets walks the whole visual context tree, so it runs once, after every offset is kept.
        if kept_any_scroll_offset {
            resolve_sticky_offsets(
                self.scene.current_visual_context_tree(),
                &mut self.scroll_state_snapshot,
            );
        }
        if !clamped_scroll_offsets.is_empty() {
            self.store_pending_async_scroll_offsets(&clamped_scroll_offsets, None);
        }
        viewport_scroll_offset
    }

    pub(super) fn store_pending_async_scroll_offsets(
        &mut self,
        scroll_offsets: &[AsyncScrollOffset],
        operation_id: Option<AsyncScrollOperationID>,
    ) {
        self.scene.forget_whether_animated_content_may_affect_viewport();
        let mut stamped_scroll_offsets = Vec::with_capacity(scroll_offsets.len());
        for mut scroll_offset in scroll_offsets.iter().cloned() {
            scroll_offset.applied_scroll_request_id = self
                .applied_scroll_request_ids
                .get(&scroll_offset.stable_node_id)
                .copied()
                .unwrap_or(0);
            scroll_offset.user_input_scroll_count = self
                .user_input_scroll_counts
                .get(&scroll_offset.stable_node_id)
                .copied()
                .unwrap_or(0);
            scroll_offset.context_user_input_scroll_count = self.context_user_input_scroll_count;
            stamped_scroll_offsets.push(scroll_offset);
        }
        self.async_scroll_update_outbox
            .record_scroll_offsets(&stamped_scroll_offsets);
        self.scroll_offsets_differ_from_last_frame = true;
        if let Some(id) = operation_id {
            self.async_scroll_update_outbox.complete_operation(id);
        }
    }
}
