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

    pub fn set_scroll_capabilities(&mut self, capabilities: &ScrollCapabilities, now: MonotonicTime) -> bool {
        self.presentation_synthesizes_scroll_momentum = capabilities.synthesizes_scroll_momentum;
        self.elastic_overscroll_controller
            .set_enabled(capabilities.scrolls_elastically);
        self.scrollbar_track_press_jumps = capabilities.scrollbar_track_press_jumps;
        self.wheel_gesture_routing
            .set_touchpad_gesture_end(capabilities.touchpad_gesture_end);

        let visibility_changed = self
            .scrollbar_controller
            .set_scrollbars_always_shown(capabilities.scrollbars_always_shown, now);
        if visibility_changed {
            self.update_scrollbar_visibility(now);
        }
        visibility_changed
    }

    pub fn advance_elastic_overscroll_rebounds(&mut self, frame_time: MonotonicTime) -> Option<IntRect> {
        if !self.elastic_overscroll_controller.advance_rebounds(frame_time) {
            return None;
        }
        Some(self.async_scrolling_viewport_rect)
    }

    /// Where the first step of a wheel gesture at the position, by the delta in CSS pixels, would go.
    pub fn route_first_wheel_step_for_testing(
        &mut self,
        position: FloatPoint,
        delta_in_css_pixels: FloatPoint,
        nested_context_regions: NestedContextRegions,
    ) -> FirstWheelStepRouteForTesting {
        self.prepare_wheel_hit_test_targets();
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
                &self.scroll_state_snapshot,
                position,
                delta,
                nested_context_regions,
            ),
            document_id,
        }
    }

    /// The installed scrolling state, its routing admission, and how far the given scroller is stretched, as JSON.
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
        let user_scroll_gesture_state = self.user_scroll_gesture_state();
        if !self.async_scroll_update_outbox.has_updates(&user_scroll_gesture_state) {
            return;
        }
        let mut updates = self.async_scroll_update_outbox.take_updates(user_scroll_gesture_state);
        updates.document_id = self.async_scroll_tree.document_id();
        self.host.post_to_web_content(
            self.web_content_client_id,
            compositor_web_content_client::messages::AsyncScrollUpdates {
                context_id: self.context_id,
                updates,
            },
        );
    }

    /// Returns whether the overlays need a frame of their own to be drawn at the new viewport size or pixel ratio. The
    /// page presents a frame at a new size, but the paused debugger's page does not paint again, and a new pixel ratio
    /// alone need not repaint the page.
    pub fn viewport_size_updated(&mut self, viewport_size: IntSize, device_pixel_ratio: f64) -> bool {
        let only_the_pixel_ratio_changed =
            viewport_size == self.viewport_size && device_pixel_ratio != self.device_pixel_ratio;
        if self.viewport_size != viewport_size {
            self.keyboard_scroll_controller.forget_target();
        }
        self.viewport_size = viewport_size;
        self.device_pixel_ratio = device_pixel_ratio;
        let ui_overlays = self.overlays.ui_overlays();
        ui_overlays.paused_debugger.is_some()
            || (only_the_pixel_ratio_changed && ui_overlays.picture_in_picture_controls.is_some())
    }

    /// Answers whether the overlays changed.
    pub fn set_ui_overlays(&mut self, ui_overlays: UIOverlays, now: MonotonicTime) -> bool {
        let shows_paused_debugger = ui_overlays.paused_debugger.is_some();
        let changed = self.overlays.set_ui_overlays(ui_overlays, now);
        if changed && shows_paused_debugger {
            self.end_keyboard_scroll_gesture();
        }
        changed
    }

    pub fn set_compositor_layers_overlay_needs_frames_until(&mut self, until: Option<MonotonicTime>) {
        self.overlays.set_compositor_layers_overlay_needs_frames_until(until);
    }

    /// Advances the fades of the overlays to a frame, and answers whether any of them changed. The frame finds what
    /// changed by comparing what the overlays draw over it with what they drew over the frame before.
    pub fn advance_overlay_fades(&mut self, frame_time: MonotonicTime) -> bool {
        self.overlays.advance_fades(frame_time)
    }

    /// Whether the compositor layers overlay needs the frame, which rasters no tile again for that.
    pub fn compositor_layers_overlay_needs_frame(&mut self, frame_time: MonotonicTime) -> bool {
        self.overlays.compositor_layers_overlay_needs_frame(frame_time)
    }

    pub fn viewport_rect(&self) -> IntRect {
        IntRect::new(0, 0, self.viewport_size.width, self.viewport_size.height)
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

    /// Where the compositor has scrolled each scroll node to.
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

    fn frame_inputs_as_presented_now(&mut self, now: MonotonicTime) -> FrameInputs {
        assert!(!self.sticky_offsets_are_stale);
        let stretched_scroll_state_snapshot = self.scroll_state_snapshot_stretched_by_elastic_overscrolls();
        let presented_scroll_state_snapshot =
            stretched_scroll_state_snapshot.unwrap_or_else(|| self.scroll_state_snapshot.clone());
        let scrollbars = self
            .scrollbar_controller
            .scrollbars_of_frame(&presented_scroll_state_snapshot);
        let overlays = self.overlays.picture_of_frame(
            self.viewport_size,
            self.device_pixel_ratio,
            &self.host.overlay_label_shaper(self.context_id),
        );
        let raster = self.scene.raster_of_frame(self.viewport_rect());
        FrameInputs {
            visual_context_tree: raster.visual_context_tree,
            raster_to_frame: raster.raster_to_frame,
            layer_plan: raster.layer_plan,
            raster_scale: raster.raster_scale,
            presented_scroll_state_snapshot,
            scrollbars,
            painted_at: now,
            overlays,
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

    /// Advances the fades of the scrollbars to a frame, and answers whether any of them changed. The frame finds what
    /// changed in the content hashes of their records.
    pub fn advance_scrollbar_fades(&mut self, frame_time: MonotonicTime) -> bool {
        if self.scene.visual_context_tree().is_none() {
            return false;
        }
        let some_scrollbar_changed = self.scrollbar_controller.advance_fades(frame_time);
        self.report_scrollbar_visibility_changes();
        some_scrollbar_changed
    }

    /// Each frame shows the scrollbars as of when it is painted, so this updates which of them are shown. Empty until a
    /// display list is installed.
    pub fn frame_inputs(&mut self, now: MonotonicTime) -> Option<FrameInputs> {
        self.update_scrollbar_visibility(now);
        self.scene.display_list().as_ref()?;
        self.ask_render_clock_for_display_list_where_scrolled_past_its_window();
        Some(self.frame_inputs_as_presented_now(now))
    }

    fn ask_render_clock_for_display_list_where_scrolled_past_its_window(&mut self) {
        if !self.has_async_scrolling_state {
            return;
        }
        let (Some(display_list), Some(visual_context_tree)) =
            (self.scene.display_list(), self.scene.visual_context_tree())
        else {
            return;
        };
        let Some(window) = display_list.window() else {
            return;
        };
        let asks = self
            .display_list_coverage_watch
            .asks_for_display_list_where_scrolled(&ShownOfDisplayList {
                display_list_id: display_list.id(),
                window,
                visual_context_tree: visual_context_tree.as_arc(),
                scroll_state_snapshot: &self.scroll_state_snapshot,
                viewport_size: self.viewport_size,
            });
        if asks {
            self.host.post_to_render_clock_of_web_content(
                self.web_content_client_id,
                render_clock_client::messages::DisplayListCoverageRunsOut {
                    context_id: self.context_id,
                    scroll_offsets: self.scroll_offsets(),
                },
            );
        }
    }

    pub fn frame_inputs_for_screenshot(&mut self, now: MonotonicTime) -> ScreenshotInputs {
        assert!(self.scene.display_list().is_some());
        // Screenshot requests can arrive between display ticks, so sample animations and scrollbar visibility at capture
        // time.
        let capture_time = now;
        self.advance_visual_animations(capture_time);
        self.update_scrollbar_visibility(now);
        let scrollbar_fades_changed = self.advance_scrollbar_fades(capture_time);
        ScreenshotInputs {
            frame_inputs: self.frame_inputs_as_presented_now(capture_time),
            scrollbar_fades_changed,
        }
    }

    fn apply_visual_viewport_scroll_delta(&mut self, delta: FloatPoint) -> Option<FloatPoint> {
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
        self.move_visual_viewport(Some(document_id), transform, VisualViewportMoveCause::Pan);
        Some(consumed_delta)
    }

    /// Pans the visual viewport of a zoomed page by as much of a delta in screen pixels as it has room for, and returns
    /// that part of the delta.
    pub(super) fn pan_visual_viewport_by(&mut self, delta_in_screen_pixels: FloatPoint) -> FloatPoint {
        self.apply_visual_viewport_scroll_delta(delta_in_screen_pixels)
            .unwrap_or_default()
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

    /// The main thread asks for every scroll of its own, so the offsets a snapshot of its scroll state has for the
    /// boxes this context already scrolls are what it last heard of them. Only a box new to this context takes its
    /// offset from the snapshot.
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
            let kept_scroll_offset = self.async_scroll_tree.set_scroll_offset_of_node(
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
                clamped_scroll_offsets.push((node.stable_node_id, kept_scroll_offset));
            }
        }
        // Resolving sticky offsets walks the whole visual context tree, so it runs once, after every offset is kept.
        if kept_any_scroll_offset {
            self.sticky_offsets_are_stale = true;
            self.resolve_stale_sticky_offsets();
        }
        for (stable_node_id, scroll_offset) in clamped_scroll_offsets {
            self.scroll_snap_controller.did_move_scroller(stable_node_id);
            self.record_scroll_offset_report(stable_node_id, scroll_offset);
        }
        viewport_scroll_offset
    }

    pub(super) fn resolve_stale_sticky_offsets(&mut self) {
        if !self.sticky_offsets_are_stale {
            return;
        }
        resolve_sticky_offsets(
            self.scene.current_visual_context_tree(),
            &mut self.scroll_state_snapshot,
        );
        self.sticky_offsets_are_stale = false;
    }

    pub(super) fn record_scroll_offset_report(
        &mut self,
        stable_node_id: AsyncScrollNodeStableID,
        compositor_scroll_offset: FloatPoint,
    ) {
        self.record_scroll_offset_report_of_scroll(stable_node_id, compositor_scroll_offset, Default::default(), None);
    }

    pub(super) fn record_relative_scroll_offset_report(
        &mut self,
        stable_node_id: AsyncScrollNodeStableID,
        compositor_scroll_offset: FloatPoint,
        relative_scroll_delta: FloatPoint,
    ) {
        self.record_scroll_offset_report_of_scroll(
            stable_node_id,
            compositor_scroll_offset,
            relative_scroll_delta,
            None,
        );
    }

    pub(super) fn record_visual_viewport_offset_report(
        &mut self,
        document_id: UniqueNodeId,
        visual_viewport_transform: &TransformWithOrigin,
    ) {
        self.record_scroll_offset_report_of_scroll(
            visual_viewport_stable_node_id(document_id),
            visual_viewport_scroll_offset(visual_viewport_transform),
            Default::default(),
            Some(visual_viewport_transform.matrix.elements[0][0]),
        );
    }

    fn record_scroll_offset_report_of_scroll(
        &mut self,
        stable_node_id: AsyncScrollNodeStableID,
        compositor_scroll_offset: FloatPoint,
        last_relative_scroll_delta: FloatPoint,
        visual_viewport_scale: Option<f32>,
    ) {
        self.async_scroll_update_outbox
            .record_scroll_offset(&AsyncScrollOffset {
                stable_node_id,
                compositor_scroll_offset,
                last_relative_scroll_delta,
                visual_viewport_scale,
                applied_scroll_request_id: self
                    .applied_scroll_request_ids
                    .get(&stable_node_id)
                    .copied()
                    .unwrap_or(0),
                user_input_scroll_count: self.user_input_scroll_counts.get(&stable_node_id).copied().unwrap_or(0),
            });
        self.scroll_offsets_differ_from_last_frame = true;
    }
}
