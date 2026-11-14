/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The stubs of the messages of WebContent and of a presentation that reach the scroll and input logic: what their
//! sender may ask for, and the answer a synchronous one gets.

use super::{CompositorCore, ContextOwnerCheckResult};
use crate::host::{PresentationId, WebContentClientId};
use crate::nested_display_lists::{NestedDisplayListStore, ReferencedResources};
use libcompositing_rust::display_list::replay::{ReplayFrame, ReplaySelection, replay_display_list};
use libcompositing_rust::ipc_types::compositor_presentation_client::messages::DidNotDispatchInputEvent;
use libcompositing_rust::ipc_types::compositor_presentation_server::messages::{
    HandleAndDispatchKeyEvent, HandleAndDispatchMouseEvent, HandleAndDispatchPinchEvent,
};
use libcompositing_rust::ipc_types::compositor_web_content_server::messages::{
    ApplyScrollRequests, BeginInputTraceForTesting, BeginInputTraceForTestingResponse,
    ClearPerTestInputStateForTesting, ClearPerTestInputStateForTestingResponse, InvalidateKeyboardScrollState,
    InvalidateKeyboardScrollStateResponse, InvalidateWheelEventListenerState, RasterizeDisplayList,
    RasterizeDisplayListResponse, RouteFirstWheelStepForTesting, RouteFirstWheelStepForTestingResponse,
    ScrollStateForTesting, ScrollStateForTestingResponse, TakeInputTraceForTesting, TakeInputTraceForTestingResponse,
    TakePendingAsyncScrollUpdates, TakePendingAsyncScrollUpdatesResponse, TakeWheelEventFromMainThread,
    TakeWheelEventFromMainThreadResponse, UpdateDisplayList, UpdateScrollState, UpdateVisualContextTree,
    WheelEventCanceled, WheelEventLetThrough, WheelEventLetThroughResponse,
};
use libcompositing_rust::ipc_types::{
    Compositing_WheelEventAnswer as WheelEventAnswer, Compositing_WheelStepLetThrough as WheelStepLetThrough,
    compositor_presentation_server, compositor_web_content_server,
};
use libcompositing_rust::scrolling::async_scrolling_state::AsyncScrollOperationID;
use libcompositing_rust::scrolling::{AccumulatedVisualContextTree, DisplayList};
use libcompositing_rust::scrolling::{
    CompositorContextId, KeyEvent, KeyModifier, MouseButton, MouseEvent, MouseEventType, PendingAsyncScrollUpdates,
    PinchEvent, ScrollRequest, ScrollStateSnapshot,
};
use libcompositing_rust::time::MonotonicTime;
use libgfx_rust::FloatPoint;
use libipc_rust::Misbehavior;
use std::io::Write;
use std::rc::Rc;

// The time a display list takes to be taken in, which WebContent's time to publish it is logged next to.
fn display_list_timing_enabled() -> bool {
    static ENABLED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ENABLED.get_or_init(|| {
        std::env::var_os("LADYBIRD_DISPLAY_LIST_TIMING").is_some_and(|value| !value.is_empty() && value != "0")
    })
}

const NESTED_DISPLAY_LISTS_THAT_NEST_THEMSELVES: &str = "WebContent published display lists that nest themselves";
const DISPLAY_LIST_WITH_UNUSABLE_SCROLLING_STATE: &str =
    "WebContent published a display list with scrolling state the compositor cannot use";

// The main thread allocates the ids of the scrolls it requests from here up, so that they never meet the ids the
// compositor allocates for scrolls of its own.
const FIRST_OPERATION_ID_THE_MAIN_THREAD_REQUESTS: AsyncScrollOperationID = 1 << 63;

// Wheel deltas are scrolled by as floats.
fn is_valid_wheel_delta(delta: f64) -> bool {
    delta.is_finite() && delta.abs() <= f32::MAX as f64
}

fn is_valid_wheel_event(event: &MouseEvent) -> bool {
    event.r#type == MouseEventType::MouseWheel
        && is_valid_wheel_delta(event.wheel_delta_x)
        && is_valid_wheel_delta(event.wheel_delta_y)
}

fn is_valid_wheel_step_let_through(step: &WheelStepLetThrough) -> bool {
    step.position.x.is_finite()
        && step.position.y.is_finite()
        && step.delta_in_css_pixels.x.is_finite()
        && step.delta_in_css_pixels.y.is_finite()
}

fn is_valid_scroll_request(scroll_request: &ScrollRequest) -> bool {
    let is_finite = |point: FloatPoint| point.x.is_finite() && point.y.is_finite();
    if scroll_request
        .jump_offset
        .is_some_and(|jump_offset| !is_finite(jump_offset))
    {
        return false;
    }
    if let Some(relative_scroll) = &scroll_request.relative_scroll {
        if !is_finite(relative_scroll.delta)
            || !is_finite(relative_scroll.lower_bound)
            || !is_finite(relative_scroll.upper_bound)
        {
            return false;
        }
        if relative_scroll.lower_bound.x > relative_scroll.upper_bound.x
            || relative_scroll.lower_bound.y > relative_scroll.upper_bound.y
        {
            return false;
        }
    }
    if let Some(scale) = scroll_request.visual_viewport_scale
        && (!scale.is_finite() || scale <= 0.0)
    {
        return false;
    }
    if let Some(smooth_scroll) = &scroll_request.smooth_scroll {
        if !is_finite(smooth_scroll.destination_offset) {
            return false;
        }
        if !(smooth_scroll.relative_lower_bound.x <= smooth_scroll.relative_upper_bound.x
            && smooth_scroll.relative_lower_bound.y <= smooth_scroll.relative_upper_bound.y)
        {
            return false;
        }
        // WebContent names the compositor's own scrolls only to head them somewhere else.
        if smooth_scroll.operation_id < FIRST_OPERATION_ID_THE_MAIN_THREAD_REQUESTS
            && !smooth_scroll.retargets_operation
        {
            return false;
        }
    }
    true
}

// NB: The IPC decoder already rejects key codes, event types, wheel delta precisions and scroll gesture phases that do
//     not exist, and pinch scales that are not finite. These check the rest of what the UI sends.

fn is_valid_modifiers(modifiers: KeyModifier) -> bool {
    (modifiers & !KeyModifier::Mod_Mask).is_empty()
}

fn is_valid_key_event(event: &KeyEvent) -> bool {
    is_valid_modifiers(event.modifiers)
        && event.code_point <= 0x10ffff
        // Only the compositor sets this, on its way to WebContent.
        && !event.async_scroll_performed_default_action
}

fn is_valid_mouse_event(event: &MouseEvent) -> bool {
    let all_buttons = MouseButton::Primary
        | MouseButton::Secondary
        | MouseButton::Middle
        | MouseButton::Backward
        | MouseButton::Forward;
    [
        MouseButton::None,
        MouseButton::Primary,
        MouseButton::Secondary,
        MouseButton::Middle,
        MouseButton::Backward,
        MouseButton::Forward,
    ]
    .contains(&event.button)
        && (event.buttons & !all_buttons).is_empty()
        && is_valid_modifiers(event.modifiers)
        && is_valid_wheel_delta(event.wheel_delta_x)
        && is_valid_wheel_delta(event.wheel_delta_y)
        && is_valid_wheel_delta(event.wheel_tick_120ths_x)
        && is_valid_wheel_delta(event.wheel_tick_120ths_y)
        && event.click_count >= 0
        // Only the compositor sets these, on its way to WebContent.
        && !event.async_scroll_performed_default_action
        && event.scrollbar_dragged_by_compositor.is_none()
        && event.wheel_gesture_mark.is_none()
}

fn is_valid_pinch_event(event: &PinchEvent) -> bool {
    is_valid_modifiers(event.modifiers)
        // Only the compositor sets this, on its way to WebContent.
        && !event.async_zoom_performed_default_action
}

impl CompositorCore {
    fn context_is_owned_by(
        &self,
        context_id: CompositorContextId,
        client_id: WebContentClientId,
    ) -> Result<bool, Misbehavior> {
        match self.check_context_owner(context_id, client_id) {
            ContextOwnerCheckResult::OwnedByClient => Ok(true),
            ContextOwnerCheckResult::ContextUnavailable => Ok(false),
            ContextOwnerCheckResult::ConflictingOwner => Err(Misbehavior(
                "WebContent tried to use a compositor context owned by another connection",
            )),
        }
    }

    fn is_bound_to(&self, context_id: CompositorContextId, presentation_id: PresentationId) -> bool {
        self.context_if_present(context_id)
            .is_some_and(|context| context.borrow().bound_presentation() == Some(presentation_id))
    }
}

pub struct MessagesFromWebContent<'a> {
    pub core: &'a CompositorCore,
    pub client_id: WebContentClientId,
    pub now: MonotonicTime,
}

impl MessagesFromWebContent<'_> {
    fn context_is_owned_by_sender(&self, context_id: CompositorContextId) -> Result<bool, Misbehavior> {
        self.core.context_is_owned_by(context_id, self.client_id)
    }
}

impl compositor_web_content_server::Stub for MessagesFromWebContent<'_> {
    fn update_display_list(&mut self, message: UpdateDisplayList) -> Result<(), Misbehavior> {
        let started_at = std::time::Instant::now();
        let tape_size = message.tape_size;
        let storage = match self.core.raster().receive_display_list_storage(
            message.display_list_buffer,
            message.tape_size,
            message.run_count,
        ) {
            Ok(storage) => storage,
            Err(error) => {
                let _ = writeln!(
                    std::io::stderr(),
                    "Compositor: Rejecting display list from WebContent: {error}"
                );
                return Err(Misbehavior(
                    "WebContent published a display list its shared buffer does not hold",
                ));
            }
        };
        let adopt_time = started_at.elapsed();
        if !self.context_is_owned_by_sender(message.context_id)? {
            return Ok(());
        }
        let properties = message.display_list_properties;
        let tree = message.visual_context_tree.0;
        if properties.compatible_visual_context_tree_structural_epoch != tree.structural_epoch {
            let _ = writeln!(
                std::io::stderr(),
                "Compositor: Dropping inconsistent display list update (display list epoch {}, tree epoch {})",
                properties.compatible_visual_context_tree_structural_epoch,
                tree.structural_epoch
            );
            return Ok(());
        }
        if !tree.display_list_references_only_live_nodes(storage.command_runs()) {
            let _ = writeln!(
                std::io::stderr(),
                "Compositor: Dropping display list update that names nodes its visual context tree does not hold"
            );
            return Ok(());
        }
        let Ok(scroll_state_snapshot) = ScrollStateSnapshot::from_encoded_bytes(&message.scroll_state) else {
            return Err(Misbehavior(DISPLAY_LIST_WITH_UNUSABLE_SCROLLING_STATE));
        };
        let display_list = DisplayList::new(
            storage,
            properties.id,
            properties.compatible_visual_context_tree_structural_epoch,
            message.async_scrolling_metadata,
            properties.surface_clear_color,
            properties.root_group_color,
        );
        if !self.core.install_display_list_update(
            message.context_id,
            Rc::new(display_list),
            AccumulatedVisualContextTree::from_arc(tree),
            scroll_state_snapshot,
            self.now,
        ) {
            return Err(Misbehavior(DISPLAY_LIST_WITH_UNUSABLE_SCROLLING_STATE));
        }
        if !self
            .core
            .apply_resource_transaction(message.context_id, message.resource_transaction)
        {
            return Err(Misbehavior(NESTED_DISPLAY_LISTS_THAT_NEST_THEMSELVES));
        }
        if display_list_timing_enabled() {
            let _ = writeln!(
                std::io::stderr(),
                "DISPLAY_LIST_RECEIVE bytes={tape_size} adopt={} µs install={} µs",
                adopt_time.as_micros(),
                (started_at.elapsed() - adopt_time).as_micros()
            );
        }
        Ok(())
    }

    fn update_visual_context_tree(&mut self, message: UpdateVisualContextTree) -> Result<(), Misbehavior> {
        let unusable_scroll_state =
            Misbehavior("WebContent published a visual context tree with a scroll state the compositor cannot use");
        let Ok(scroll_state_snapshot) = ScrollStateSnapshot::from_encoded_bytes(&message.scroll_state) else {
            return Err(unusable_scroll_state);
        };
        if !scroll_state_snapshot.has_only_finite_offsets() {
            return Err(unusable_scroll_state);
        }
        if !self.context_is_owned_by_sender(message.context_id)? {
            return Ok(());
        }
        let tree_was_replaced = self.core.update_visual_context_tree(
            message.context_id,
            AccumulatedVisualContextTree::from_arc(message.visual_context_tree.0),
            scroll_state_snapshot,
            message.keyboard_scroll_state,
            self.now,
        );
        if tree_was_replaced
            && !self
                .core
                .apply_resource_transaction(message.context_id, message.resource_transaction)
        {
            return Err(Misbehavior(NESTED_DISPLAY_LISTS_THAT_NEST_THEMSELVES));
        }
        Ok(())
    }

    fn rasterize_display_list(
        &mut self,
        message: RasterizeDisplayList,
    ) -> Result<RasterizeDisplayListResponse, Misbehavior> {
        let raster = self.core.raster();
        let (client, token) = (self.client_id.0, message.token);
        let not_rasterized = |reason: &str| {
            let _ = writeln!(
                std::io::stderr(),
                "Compositor: Not rasterizing display list from WebContent: {reason}"
            );
            raster.discard_staged_rasterization(client, token);
            Ok(RasterizeDisplayListResponse { success: false })
        };
        let storage = match raster.receive_display_list_storage(
            message.display_list_buffer,
            message.tape_size,
            message.run_count,
        ) {
            Ok(storage) => storage,
            Err(error) => {
                let _ = writeln!(
                    std::io::stderr(),
                    "Compositor: Rejecting display list to rasterize from WebContent: {error}"
                );
                raster.discard_staged_rasterization(client, token);
                return Err(Misbehavior(
                    "WebContent asked to rasterize a display list its shared buffer does not hold",
                ));
            }
        };
        let transaction = message.nested_display_lists;
        if transaction.removes_anything() || !transaction.video_sinks.is_empty() {
            return not_rasterized("Resource transaction holds more than the display lists it nests");
        }
        let mut nested_display_lists = NestedDisplayListStore::default();
        for display_list in transaction.display_lists {
            nested_display_lists.add(display_list);
        }
        let properties = message.display_list_properties;
        let tree = message.visual_context_tree.0;
        if properties.compatible_visual_context_tree_structural_epoch != tree.structural_epoch
            || !tree.display_list_references_only_live_nodes(storage.command_runs())
        {
            return not_rasterized("Display list does not match its visual context tree");
        }
        let mut resources = ReferencedResources::default();
        nested_display_lists.collect_referenced_resources(&storage, &tree, &mut resources);
        if !nested_display_lists.nested_display_lists_are_replayable(&resources) {
            return not_rasterized("Nested display list is missing or refers to itself");
        }
        let ids = |set: &std::collections::HashSet<u64>| set.iter().copied().collect::<Vec<u64>>();
        if !raster.staged_rasterization_has_resources(
            client,
            token,
            &ids(&resources.font_ids),
            &ids(&resources.image_frame_ids),
        ) {
            return not_rasterized("Display list draws resources that were not staged for it");
        }
        let time_ns = self.now.nanoseconds();
        let success = raster.rasterize_staged(client, token, |painter| {
            let frame = ReplayFrame::unscrolled(time_ns, &nested_display_lists);
            replay_display_list(
                &tree,
                storage.bytes(),
                storage.command_runs(),
                storage.effect_clip_plan(&tree),
                properties.root_group_color,
                &frame,
                ReplaySelection::default(),
                painter,
            );
        });
        Ok(RasterizeDisplayListResponse { success })
    }

    fn update_scroll_state(&mut self, message: UpdateScrollState) -> Result<(), Misbehavior> {
        let Ok(scroll_state_snapshot) = ScrollStateSnapshot::from_encoded_bytes(&message.scroll_state) else {
            return Err(Misbehavior("Invalid scroll state"));
        };
        if !scroll_state_snapshot.has_only_finite_offsets() {
            return Err(Misbehavior("Invalid scroll state"));
        }
        if self.context_is_owned_by_sender(message.context_id)? {
            self.core
                .update_scroll_state(message.context_id, scroll_state_snapshot, message.keyboard_scroll_state);
        }
        Ok(())
    }

    fn invalidate_wheel_event_listener_state(
        &mut self,
        message: InvalidateWheelEventListenerState,
    ) -> Result<(), Misbehavior> {
        if self.context_is_owned_by_sender(message.context_id)? {
            self.core
                .invalidate_wheel_event_listener_state(message.context_id, message.generation);
        }
        Ok(())
    }

    fn invalidate_keyboard_scroll_state(
        &mut self,
        message: InvalidateKeyboardScrollState,
    ) -> Result<InvalidateKeyboardScrollStateResponse, Misbehavior> {
        if self.context_is_owned_by_sender(message.context_id)? {
            self.core
                .invalidate_keyboard_scroll_state(message.context_id, message.generation);
        }
        Ok(InvalidateKeyboardScrollStateResponse)
    }

    fn take_wheel_event_from_main_thread(
        &mut self,
        message: TakeWheelEventFromMainThread,
    ) -> Result<TakeWheelEventFromMainThreadResponse, Misbehavior> {
        let answer = if self.context_is_owned_by_sender(message.context_id)? && is_valid_wheel_event(&message.event) {
            self.core.take_wheel_event_from_main_thread(
                message.context_id,
                &message.event,
                message.operation_tracking,
                message.routing,
                self.now,
            )
        } else {
            WheelEventAnswer::default()
        };
        Ok(TakeWheelEventFromMainThreadResponse { answer })
    }

    fn wheel_event_let_through(
        &mut self,
        message: WheelEventLetThrough,
    ) -> Result<WheelEventLetThroughResponse, Misbehavior> {
        let answer =
            if self.context_is_owned_by_sender(message.context_id)? && is_valid_wheel_step_let_through(&message.step) {
                self.core.scroll_for_wheel_event_the_page_let_through(
                    message.context_id,
                    &message.step,
                    message.operation_tracking,
                    self.client_id,
                    self.now,
                )
            } else {
                WheelEventAnswer::default()
            };
        Ok(WheelEventLetThroughResponse { answer })
    }

    fn wheel_event_canceled(&mut self, message: WheelEventCanceled) -> Result<(), Misbehavior> {
        if self.context_is_owned_by_sender(message.context_id)? {
            self.core
                .forget_wheel_gesture_the_page_canceled(message.context_id, message.wheel_gesture_id);
        }
        Ok(())
    }

    fn apply_scroll_requests(&mut self, message: ApplyScrollRequests) -> Result<(), Misbehavior> {
        if !self.context_is_owned_by_sender(message.context_id)? {
            return Ok(());
        }
        if !message.scroll_requests.iter().all(is_valid_scroll_request) {
            return Err(Misbehavior("Invalid scroll request"));
        }
        self.core
            .apply_scroll_requests(message.context_id, &message.scroll_requests, self.now);
        Ok(())
    }

    fn scroll_state_for_testing(
        &mut self,
        message: ScrollStateForTesting,
    ) -> Result<ScrollStateForTestingResponse, Misbehavior> {
        let scroll_state = if self.context_is_owned_by_sender(message.context_id)? {
            self.core
                .scroll_state_for_testing(message.context_id, message.stretched_scroller)
        } else {
            String::new()
        };
        Ok(ScrollStateForTestingResponse {
            scroll_state: scroll_state.into(),
        })
    }

    fn route_first_wheel_step_for_testing(
        &mut self,
        message: RouteFirstWheelStepForTesting,
    ) -> Result<RouteFirstWheelStepForTestingResponse, Misbehavior> {
        let route = if self.context_is_owned_by_sender(message.context_id)? {
            self.core.route_first_wheel_step_for_testing(
                message.context_id,
                message.position,
                message.delta_in_css_pixels,
            )
        } else {
            None
        };
        Ok(match route {
            Some(route) => RouteFirstWheelStepForTestingResponse {
                decision: Some(route.route.decision),
                scroller: route.route.scroller,
                document_id: route.document_id,
            },
            None => RouteFirstWheelStepForTestingResponse {
                decision: None,
                scroller: None,
                document_id: None,
            },
        })
    }

    fn begin_input_trace_for_testing(
        &mut self,
        message: BeginInputTraceForTesting,
    ) -> Result<BeginInputTraceForTestingResponse, Misbehavior> {
        if self.context_is_owned_by_sender(message.context_id)? {
            self.core.begin_input_trace_for_testing(message.context_id);
        }
        Ok(BeginInputTraceForTestingResponse)
    }

    fn clear_per_test_input_state_for_testing(
        &mut self,
        message: ClearPerTestInputStateForTesting,
    ) -> Result<ClearPerTestInputStateForTestingResponse, Misbehavior> {
        if self.context_is_owned_by_sender(message.context_id)? {
            self.core.clear_per_test_input_state_for_testing(message.context_id);
        }
        Ok(ClearPerTestInputStateForTestingResponse)
    }

    fn take_input_trace_for_testing(
        &mut self,
        message: TakeInputTraceForTesting,
    ) -> Result<TakeInputTraceForTestingResponse, Misbehavior> {
        let records = if self.context_is_owned_by_sender(message.context_id)? {
            self.core.take_input_trace_for_testing(message.context_id)
        } else {
            Vec::new()
        };
        Ok(TakeInputTraceForTestingResponse { records })
    }

    fn take_pending_async_scroll_updates(
        &mut self,
        message: TakePendingAsyncScrollUpdates,
    ) -> Result<TakePendingAsyncScrollUpdatesResponse, Misbehavior> {
        let updates = if self.context_is_owned_by_sender(message.context_id)? {
            self.core.take_pending_async_scroll_updates(message.context_id)
        } else {
            PendingAsyncScrollUpdates::default()
        };
        Ok(TakePendingAsyncScrollUpdatesResponse { updates })
    }
}

pub struct MessagesFromPresentation<'a> {
    pub core: &'a CompositorCore,
    pub presentation_id: PresentationId,
    pub now: MonotonicTime,
}

impl MessagesFromPresentation<'_> {
    fn sender_gives_input_to(&self, context_id: CompositorContextId, event_id: u64) -> bool {
        if self.core.is_bound_to(context_id, self.presentation_id) {
            return true;
        }
        self.core
            .host
            .post_to_presentation(self.presentation_id, DidNotDispatchInputEvent { context_id, event_id });
        false
    }
}

impl compositor_presentation_server::Stub for MessagesFromPresentation<'_> {
    fn handle_and_dispatch_key_event(&mut self, message: HandleAndDispatchKeyEvent) -> Result<(), Misbehavior> {
        if !is_valid_key_event(&message.event) {
            return Err(Misbehavior("Invalid key event"));
        }
        if self.sender_gives_input_to(message.context_id, message.event.id) {
            self.core.handle_and_dispatch_key_event(
                message.context_id,
                message.event,
                message.allow_keyboard_scroll,
                self.now,
            );
        }
        Ok(())
    }

    fn handle_and_dispatch_pinch_event(&mut self, message: HandleAndDispatchPinchEvent) -> Result<(), Misbehavior> {
        if !is_valid_pinch_event(&message.event) {
            return Err(Misbehavior("Invalid pinch event"));
        }
        if self.sender_gives_input_to(message.context_id, message.event.id) {
            self.core
                .handle_and_dispatch_pinch_event(message.context_id, message.event);
        }
        Ok(())
    }

    fn handle_and_dispatch_mouse_event(&mut self, message: HandleAndDispatchMouseEvent) -> Result<(), Misbehavior> {
        if !is_valid_mouse_event(&message.event) {
            return Err(Misbehavior("Invalid mouse event"));
        }
        if self.sender_gives_input_to(message.context_id, message.event.id) {
            self.core
                .handle_and_dispatch_mouse_event(message.context_id, message.event, self.now);
        }
        Ok(())
    }
}
