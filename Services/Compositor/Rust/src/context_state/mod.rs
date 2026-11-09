/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The scroll and input state of one compositor context.

mod frames_and_viewport;
mod gestures_and_scrollbars;
mod keys_and_pointer;
mod scrolling;
mod wheel;

use crate::async_scroll_update_outbox::{AsyncScrollUpdateOutbox, UserScrollGestureState};
use crate::context_scene::ContextScene;
use crate::elastic_overscroll_controller::ElasticOverscrollController;
use crate::frame_inputs::FrameInputs;
use crate::host::{ContextTimer, Host, PendingFrame, PresentationId, WebContentClientId};
use crate::input_event::is_keyboard_scroll_key;
use crate::keyboard_scroll_controller::{KeyRelease, KeyStep, KeyboardScrollController};
use crate::scroll_fling_controller::ScrollFlingController;
use crate::scroll_snap_controller::{ScrollSnapController, SnapScrollStart, StepDecision};
use crate::scrollbar_controller::{Drag, ScrollbarController};
use crate::wheel_gesture_routing::{ResolvedLatch, WheelGestureIdAllocator, WheelGestureRouting};
use libcompositing_rust::ipc_types::{
    Compositing_AsyncScrollOperationTracking as AsyncScrollOperationTracking,
    Compositing_ContextVisibility as ContextVisibility, Compositing_RequestedRelativeScroll as RequestedRelativeScroll,
    Compositing_SmoothScrollInitiator as SmoothScrollInitiator, Web_CompositorScrollOffset as CompositorScrollOffset,
    Web_InputTraceRecord as InputTraceRecord, compositor_web_content_client, render_clock_client,
};
use libcompositing_rust::scrolling::smooth_scroll_animation::SmoothScrollAnimation;
use libcompositing_rust::scrolling::wheel_gesture_identity::TouchpadGestureEnd;
use libcompositing_rust::scrolling::*;
use libcompositing_rust::time::{Duration, MonotonicTime};
use libgfx_rust::{FloatPoint, IntRect, IntSize, Orientation, enclosing_int_rect};
use std::collections::{HashMap, HashSet};
use std::rc::Rc;

#[derive(Default)]
pub struct AsyncScrollEnqueueResult {
    pub accepted: bool,
    pub operation_id: Option<AsyncScrollOperationID>,
    // Why the compositor took or turned away a wheel step, and the scroller it routed the step to.
    pub routing_decision: Option<InputRoutingDecision>,
    pub routed_scroller: Option<AsyncScrollNodeStableID>,
}

#[derive(Default)]
pub struct AsyncScrollResult {
    pub enqueue_result: AsyncScrollEnqueueResult,
    pub frame_to_present: Option<PendingFrame>,
}

impl AsyncScrollResult {
    pub fn declined(decision: InputRoutingDecision) -> Self {
        Self {
            enqueue_result: AsyncScrollEnqueueResult {
                routing_decision: Some(decision),
                ..Default::default()
            },
            ..Default::default()
        }
    }
}

// A wheel step that goes to a context nested in this one: the first step of a gesture over the content of the
// nested context, or a step of a gesture latched to a context nested here.
pub struct NestedContextWheelStep {
    pub context_id: CompositorContextId,
    // Where the first step lands in the content of the nested context.
    pub hit: Option<NestedContextHit>,
}

// An operation that a wheel step asked to follow, whose end the scroll updates of the context report.
pub struct TrackedScrollOperation {
    pub context_id: CompositorContextId,
    pub operation_id: AsyncScrollOperationID,
}

#[derive(Default)]
pub struct ContextUpdateResult {
    pub accepted: bool,
    pub frame_to_present: Option<PendingFrame>,
    pub should_request_rendering_update: bool,
    // Whether a key event found nothing to scroll by the compositor's scroll range, which may lag behind the page's.
    pub key_found_nothing_to_scroll: bool,
    pub scrollbar_dragged_by_compositor: Option<ScrollbarDraggedByCompositor>,
    // Why a wheel or mouse event was taken or turned away, and the scroller it was routed to.
    pub routing_decision: Option<InputRoutingDecision>,
    pub routed_scroller: Option<AsyncScrollNodeStableID>,
    // How the first step of a wheel gesture went by a nested context, ahead of `routing_decision`.
    pub nested_context_hand_off: Option<InputRoutingDecision>,
    // Set instead of everything else for a wheel step that a nested context is to take.
    pub nested_context_wheel_step: Option<NestedContextWheelStep>,
    // Set, with at most the frame of the scrollbar the pointer left here, for a mouse event that the nested context
    // under the pointer is to take.
    pub nested_context_mouse_hit: Option<NestedContextHit>,
    pub tracked_scroll_operation: Option<TrackedScrollOperation>,
}

impl ContextUpdateResult {
    pub fn declined(decision: InputRoutingDecision) -> Self {
        Self {
            routing_decision: Some(decision),
            ..Default::default()
        }
    }
}

// A step of a wheel gesture at a position in this context's device pixels. The delta is in CSS pixels, with the axes
// already swapped for Shift.
pub struct WheelStep {
    pub position: FloatPoint,
    pub delta_in_css_pixels: FloatPoint,
    pub wheel_delta_precision: WheelDeltaPrecision,
    pub scroll_gesture_phase: ScrollGesturePhase,
    // The gesture the step is in, which the latch a context takes for the step is for. A context nested in another
    // one takes it for the gesture of the containing context.
    pub gesture_id: Option<u64>,
    pub operation_tracking: AsyncScrollOperationTracking,
}

// What the wheel event listeners of the page that dispatches a gesture's wheel events let through without waiting
// for the page. That page may be the page of a context nested in this one.
#[derive(Default)]
pub struct WheelEventListenerGate {
    // The page dispatched a step of the gesture and did not cancel it, and the listeners are as they were then.
    pub page_let_the_gesture_through: bool,
    // Why the listeners hold the steps of a latched gesture back for the page, if they do.
    pub reason_to_hold_latched_steps_back: Option<InputRoutingDecision>,
}

// What may hold a step back for the page before this context scrolls by it.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum WheelStepAdmission {
    // The wheel event listeners of this context's page see the step, which waits for the page if they may cancel it.
    ByThisPage,
    // A context nested in this one passed the step on. Only the page of that context sees the step.
    PassedOnByNestedContext,
    // The page let the gesture through. Nothing holds the step back, and a step that finds nothing to scroll is done
    // with all the same, so that the page is not asked about the gesture again.
    LetThroughByThePage,
}

// Where the first step of a gesture that the containing context hands in goes in this context. A nested context
// passes a step on to the document around it unless one of its scrollers takes or keeps it.
pub struct FirstWheelStepRoute {
    pub kind: FirstWheelStepRouteKind,
    pub decision: InputRoutingDecision,
    pub scroll_node_index: Option<SpatialNodeIndex>,
    pub scroller: Option<AsyncScrollNodeStableID>,
    pub nested_context_hit: Option<NestedContextHit>,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum FirstWheelStepRouteKind {
    Declined,
    ScrollNode,
    NestedContext,
    Kept,
    PassedOn,
}

// What a presenting context keeps of the contexts nested in it that the pointer is in, with positions in the device
// pixels of those contexts.
#[derive(Default)]
pub struct PointerInNestedContexts {
    // The context whose scrollbar the pointer is dragging, which takes the pointer wherever it goes until the drag
    // ends.
    pub context_with_captured_scrollbar: Option<CompositorContextId>,
    pub last_position_in_context_with_captured_scrollbar: FloatPoint,
    pub context_under_pointer: Option<CompositorContextId>,
}

// What this context noted of a wheel event as the event arrived.
pub struct NotedWheelEvent {
    pub wheel_gesture_mark: WheelGestureMark,
    // Only the first step of a gesture that moves anything goes to a nested context.
    pub is_first_wheel_event_with_delta: bool,
}

// What the presentation a context is shown through makes of the user's scrolling.
pub struct ScrollCapabilities {
    pub synthesizes_scroll_momentum: bool,
    pub scrolls_elastically: bool,
    pub scrollbar_track_press_jumps: bool,
    pub touchpad_gesture_end: TouchpadGestureEnd,
}

impl Default for ScrollCapabilities {
    fn default() -> Self {
        Self {
            synthesizes_scroll_momentum: false,
            scrolls_elastically: false,
            scrollbar_track_press_jumps: false,
            touchpad_gesture_end: TouchpadGestureEnd::AfterIdleDelay,
        }
    }
}

pub struct ScreenshotInputs {
    pub frame_inputs: FrameInputs,
    // The scrollbar fade damage that the screenshot does not present on screen.
    pub scrollbar_fade_damage_rect: IntRect,
}

struct ActiveSmoothScrollAnimation {
    stable_node_id: AsyncScrollNodeStableID,
    operation_id: AsyncScrollOperationID,
    animation: SmoothScrollAnimation,
    started_at: MonotonicTime,
    is_user_scroll: bool,
    // Whether the scroll goes to an end of the scroll range, as the Home and End keys scroll.
    heads_to_an_extent: bool,
}

struct VisualViewportScrollDelta {
    scroll_offset: AsyncScrollOffset,
    consumed_delta: FloatPoint,
}

struct KeyScroll {
    step: KeyStep,
    // Empty if every box the key could scroll is at the end of its scroll range.
    target_scroll_node_index: Option<SpatialNodeIndex>,
}

#[derive(PartialEq, Eq)]
enum VisualViewportPanning {
    // The step is in the pixels of the scroller, and only scrolls it.
    No,
    // The step is in screen pixels, and pans the visual viewport of a zoomed page before it scrolls anything.
    BeforeScrolling,
}

#[derive(Default)]
struct WheelScrollOutcome {
    operation_id: Option<AsyncScrollOperationID>,
    // The viewport to present, when the step panned the visual viewport, moved a scrolling box or started a snap
    // scroll of one.
    viewport_rect_to_present: Option<IntRect>,
    // What is presented differs only by an elastic overscroll, which WebContent has nothing to update for.
    only_elastic_overscroll_changed: bool,
}

struct NodeScrollByWheelDelta {
    outcome: WheelScrollOutcome,
    // Empty when a wheel animation or a snap scroll took the delta, and when it scrolled an ancestor of the node.
    delta_the_node_did_not_scroll_by: Option<FloatPoint>,
}

struct WheelGestureLatchAttempt {
    decision: InputRoutingDecision,
    latched_scroll_node_index: Option<SpatialNodeIndex>,
}

// A press beside the thumb that pages toward the pointer for as long as it is held.
#[derive(Clone, Copy, Default)]
struct ScrollbarPaging {
    direction: i32,
    primary_position: f32,
}

// Collects the routing decisions of input events while a test asks for them.
#[derive(Default)]
pub struct InputTraceRecorder {
    records: Option<Vec<InputTraceRecord>>,
}

impl InputTraceRecorder {
    pub fn begin(&mut self) {
        self.records = Some(Vec::new());
    }
    pub fn is_recording(&self) -> bool {
        self.records.is_some()
    }
    pub fn record(&mut self, record: InputTraceRecord) {
        // A test that never takes its trace must not grow it without bound.
        let Some(records) = self.records.as_mut().filter(|records| records.len() < 10_000) else {
            return;
        };
        records.push(record);
    }
    // Returns what was recorded since the trace began, and stops recording.
    pub fn take(&mut self) -> Vec<InputTraceRecord> {
        self.records.take().unwrap_or_default()
    }
}

pub struct ContextState {
    host: Rc<Host>,
    web_content_client_id: WebContentClientId,
    context_id: CompositorContextId,
    page_id: Option<u64>,
    bound_presentation: Option<PresentationId>,

    parent_context_id: Option<CompositorContextId>,

    scene: ContextScene,
    presents_to_client: bool,
    viewport_size: IntSize,
    paused_debugger_overlay_is_shown: bool,
    scroll_state_snapshot: ScrollStateSnapshot,

    async_scroll_tree: AsyncScrollTree,
    scrollbar_controller: ScrollbarController,
    scroll_snap_controller: ScrollSnapController,

    async_scroll_update_outbox: AsyncScrollUpdateOutbox,
    // The last of the main thread's scroll requests applied to each box, which the box's reported offsets are marked with.
    applied_scroll_request_ids: HashMap<AsyncScrollNodeStableID, u64>,
    // How many times this context scrolled each box for user input of its own, which the reports are marked with too.
    user_input_scroll_counts: HashMap<AsyncScrollNodeStableID, u64>,
    context_user_input_scroll_count: u64,
    // The boxes the last frame brought, whose offsets came from that frame's snapshot.
    scroll_nodes_seeded_by_last_frame: HashSet<AsyncScrollNodeStableID>,
    // Whether this context has scrolled boxes to offsets that WebContent's last frame does not have them at.
    scroll_offsets_differ_from_last_frame: bool,
    // The document whose visual viewport this context zoomed or panned, which it presents as it left it from then on.
    visual_viewport_document_id: Option<UniqueNodeId>,
    pinch_gesture_was_let_through: bool,
    keyboard_scroll_controller: KeyboardScrollController,
    // The boxes that the keys held since none was have scrolled, whose gesture lasts until the last of them is released.
    boxes_scrolled_by_held_keys: Vec<AsyncScrollNodeStableID>,
    wheel_gesture_routing: WheelGestureRouting,
    smooth_scroll_animations: Vec<ActiveSmoothScrollAnimation>,
    // The smooth scroll that a press beside the thumb of the captured scrollbar started.
    scrollbar_press_scroll_operation: Option<AsyncScrollOperationID>,
    scrollbar_paging: Option<ScrollbarPaging>,
    scrollbar_track_press_jumps: bool,
    presentation_synthesizes_scroll_momentum: bool,
    scrollbar_paging_timer_is_armed: bool,
    pointer_in_nested_contexts: PointerInNestedContexts,
    pointer_position: Option<FloatPoint>,
    animated_content_was_under_pointer: bool,
    scroll_fling_controller: ScrollFlingController,
    last_reported_scroll_momentum_step: Option<MouseEvent>,
    reported_scroll_momentum_was_interrupted: bool,
    elastic_overscroll_controller: ElasticOverscrollController,
    async_scrolling_viewport_rect: IntRect,
    has_async_scrolling_state: bool,

    caret_blink_cycle_start_time_ns: Option<i64>,
    input_trace: InputTraceRecorder,
    visibility: ContextVisibility,
}

// INTEROP: Holding a press beside the thumb pages again after these delays, as in Chromium and Firefox.
const SCROLLBAR_PAGING_INITIAL_DELAY: Duration = Duration::from_milliseconds(250);
const SCROLLBAR_PAGING_REPEAT_INTERVAL: Duration = Duration::from_milliseconds(50);

// INTEROP: The modifier that makes a press beside the thumb page instead of jumping there or the other way around, as in
//          Chromium and Firefox.
#[cfg(target_os = "macos")]
const SCROLLBAR_TRACK_PRESS_INVERTING_MODIFIER: KeyModifier = KeyModifier::Mod_Alt;
#[cfg(not(target_os = "macos"))]
const SCROLLBAR_TRACK_PRESS_INVERTING_MODIFIER: KeyModifier = KeyModifier::Mod_Shift;

// The offset of the visual viewport in the layout viewport, in device pixels of the content it scales.
fn visual_viewport_scroll_offset(transform: &TransformWithOrigin) -> FloatPoint {
    let scale = transform.matrix.elements[0][0];
    FloatPoint {
        x: -transform.matrix.elements[0][3],
        y: -transform.matrix.elements[1][3],
    }
    .scaled(1.0 / scale)
}

fn clamp_visual_viewport_transform_to_viewport(transform: &mut TransformWithOrigin, viewport_rect: IntRect) {
    let scale = transform.matrix.elements[0][0];
    if scale <= 1.0 {
        transform.matrix.elements[0][3] = 0.0;
        transform.matrix.elements[1][3] = 0.0;
        return;
    }
    let min_x = -(viewport_rect.width as f32) * (scale - 1.0);
    let min_y = -(viewport_rect.height as f32) * (scale - 1.0);
    transform.matrix.elements[0][3] = clamp(transform.matrix.elements[0][3], min_x, 0.0);
    transform.matrix.elements[1][3] = clamp(transform.matrix.elements[1][3], min_y, 0.0);
}

impl ContextState {
    pub fn new(
        context_id: CompositorContextId,
        page_id: Option<u64>,
        web_content_client_id: WebContentClientId,
        host: Rc<Host>,
        wheel_gesture_id_allocator: Rc<WheelGestureIdAllocator>,
        scrollbars_always_shown: bool,
    ) -> Self {
        Self {
            host,
            web_content_client_id,
            context_id,
            page_id,
            bound_presentation: Default::default(),
            parent_context_id: Default::default(),
            scene: Default::default(),
            presents_to_client: page_id.is_some(),
            viewport_size: Default::default(),
            paused_debugger_overlay_is_shown: Default::default(),
            scroll_state_snapshot: Default::default(),
            async_scroll_tree: Default::default(),
            scrollbar_controller: ScrollbarController::new(scrollbars_always_shown),
            scroll_snap_controller: Default::default(),
            async_scroll_update_outbox: Default::default(),
            applied_scroll_request_ids: Default::default(),
            user_input_scroll_counts: Default::default(),
            context_user_input_scroll_count: Default::default(),
            scroll_nodes_seeded_by_last_frame: Default::default(),
            scroll_offsets_differ_from_last_frame: Default::default(),
            visual_viewport_document_id: Default::default(),
            pinch_gesture_was_let_through: Default::default(),
            keyboard_scroll_controller: Default::default(),
            boxes_scrolled_by_held_keys: Default::default(),
            wheel_gesture_routing: WheelGestureRouting::new(wheel_gesture_id_allocator),
            smooth_scroll_animations: Default::default(),
            scrollbar_press_scroll_operation: Default::default(),
            scrollbar_paging: Default::default(),
            scrollbar_track_press_jumps: Default::default(),
            presentation_synthesizes_scroll_momentum: Default::default(),
            scrollbar_paging_timer_is_armed: Default::default(),
            pointer_in_nested_contexts: Default::default(),
            pointer_position: Default::default(),
            animated_content_was_under_pointer: Default::default(),
            scroll_fling_controller: Default::default(),
            last_reported_scroll_momentum_step: Default::default(),
            reported_scroll_momentum_was_interrupted: Default::default(),
            elastic_overscroll_controller: Default::default(),
            async_scrolling_viewport_rect: Default::default(),
            has_async_scrolling_state: Default::default(),
            caret_blink_cycle_start_time_ns: Default::default(),
            input_trace: Default::default(),
            visibility: ContextVisibility::Visible,
        }
    }
    pub fn web_content_client_id(&self) -> WebContentClientId {
        self.web_content_client_id
    }
    pub fn can_dispatch_input_to_web_content(&self) -> bool {
        self.page_id.is_some()
    }
    pub fn page_id(&self) -> Option<u64> {
        self.page_id
    }
    pub fn set_bound_presentation(&mut self, presentation_id: Option<PresentationId>) {
        self.bound_presentation = presentation_id;
    }
    pub fn bound_presentation(&self) -> Option<PresentationId> {
        self.bound_presentation
    }
    pub fn presents_to_client(&self) -> bool {
        self.presents_to_client
    }
    pub fn context_id(&self) -> CompositorContextId {
        self.context_id
    }
    pub fn parent_context_id(&self) -> Option<CompositorContextId> {
        self.parent_context_id
    }
    pub fn has_captured_scrollbar(&self) -> bool {
        self.scrollbar_controller.has_captured_scrollbar()
    }
    pub fn pointer_in_nested_contexts(&mut self) -> &mut PointerInNestedContexts {
        &mut self.pointer_in_nested_contexts
    }
    pub fn pointer_in_nested_contexts_copy(&self) -> PointerInNestedContexts {
        PointerInNestedContexts {
            context_with_captured_scrollbar: self.pointer_in_nested_contexts.context_with_captured_scrollbar,
            last_position_in_context_with_captured_scrollbar: self
                .pointer_in_nested_contexts
                .last_position_in_context_with_captured_scrollbar,
            context_under_pointer: self.pointer_in_nested_contexts.context_under_pointer,
        }
    }
    pub fn set_pointer_in_nested_contexts(&mut self, pointer: PointerInNestedContexts) {
        self.pointer_in_nested_contexts = pointer;
    }
    pub fn wheel_gesture_id(&self) -> Option<u64> {
        self.wheel_gesture_routing.gesture_id()
    }
    pub fn wheel_step_was_overtaken(&self, phase: ScrollGesturePhase, step_number: u64) -> bool {
        self.wheel_gesture_routing.step_was_overtaken(phase, step_number)
    }
    pub fn wheel_event_listener_context(&self) -> Option<CompositorContextId> {
        self.wheel_gesture_routing.wheel_event_listener_context()
    }
    pub fn set_wheel_event_listener_context(&mut self, context_id: CompositorContextId) {
        self.wheel_gesture_routing.set_wheel_event_listener_context(context_id);
    }
    pub fn note_page_let_wheel_gesture_through(&mut self, gesture_id: u64) {
        self.wheel_gesture_routing.note_page_let_gesture_through(gesture_id);
    }
    pub fn forget_page_let_wheel_gesture_through(&mut self, gesture_id: u64) {
        self.wheel_gesture_routing.forget_page_let_gesture_through(gesture_id);
    }
    pub fn take_wheel_delta_left_to_the_page(&mut self, delta_in_css_pixels: FloatPoint) -> bool {
        self.wheel_gesture_routing
            .take_delta_left_to_the_page(delta_in_css_pixels)
    }
    pub fn has_wheel_latch_for_gesture(&self, gesture_id: Option<u64>) -> bool {
        self.wheel_gesture_routing.has_latch_for_gesture(gesture_id)
    }
    pub fn nested_context_of_wheel_latch(&self) -> Option<CompositorContextId> {
        self.wheel_gesture_routing.nested_context_of_latch()
    }
    pub fn wheel_latched_scroller(&self) -> Option<AsyncScrollNodeStableID> {
        self.wheel_gesture_routing.latched_scroller()
    }
    pub fn forget_wheel_latch_of_gesture(&mut self, gesture_id: u64) {
        self.wheel_gesture_routing.clear_latch_of_gesture(gesture_id);
    }
    pub fn end_wheel_gesture_stretch(&mut self, now: MonotonicTime) {
        self.elastic_overscroll_controller.end_gesture(now);
    }
    pub fn admits_async_pinch(&self) -> bool {
        self.wheel_gesture_routing.admits_async_pinch()
    }
    pub fn pinch_gesture_was_let_through(&self) -> bool {
        self.pinch_gesture_was_let_through
    }
    pub fn user_input_scroll_count(&self) -> u64 {
        self.context_user_input_scroll_count
    }
    pub fn set_pinch_gesture_was_let_through(&mut self, was_let_through: bool) {
        self.pinch_gesture_was_let_through = was_let_through;
    }
    pub fn forget_wheel_gesture_latch(&mut self) {
        self.wheel_gesture_routing.clear_latch();
    }
    pub fn document_id(&self) -> Option<UniqueNodeId> {
        self.async_scroll_tree.document_id()
    }
    pub fn needs_animation_frames(&mut self, now: MonotonicTime) -> bool {
        self.has_active_smooth_scroll_animations()
            || self.has_active_scroll_fling()
            || self.elastic_overscroll_controller.has_rebound()
            || self.visual_animations_need_frame(now)
            || self.scrollbar_controller.fades_need_frame(now)
    }
    pub fn has_active_scroll_fling(&self) -> bool {
        self.scroll_fling_controller.is_active()
    }
    pub fn presentation_synthesizes_scroll_momentum(&self) -> bool {
        self.presentation_synthesizes_scroll_momentum
    }
    pub fn input_trace(&mut self) -> &mut InputTraceRecorder {
        &mut self.input_trace
    }
    pub fn input_trace_mut(&mut self) -> &mut InputTraceRecorder {
        &mut self.input_trace
    }
    pub fn viewport_size(&self) -> IntSize {
        self.viewport_size
    }
    pub fn visibility(&self) -> ContextVisibility {
        self.visibility
    }
    fn has_active_smooth_scroll_animations(&self) -> bool {
        !self.smooth_scroll_animations.is_empty()
    }

    pub fn is_owned_by(&self, web_content_client_id: WebContentClientId) -> bool {
        self.web_content_client_id == web_content_client_id
    }

    pub fn request_display_ticks_if_needed(&mut self, now: MonotonicTime) {
        if self.needs_animation_frames(now) {
            self.host.request_display_ticks(self.context_id);
        }
    }

    pub fn timer_fired(&mut self, timer: ContextTimer, now: MonotonicTime) -> Option<IntRect> {
        match timer {
            ContextTimer::ScrollStepGestureInput => self.end_scroll_step_gestures_whose_input_ran_out(now),
            ContextTimer::ScrollbarPaging => {
                self.scrollbar_paging_timer_is_armed = false;
                // Paging ends with the press, and with the scrollbar the press holds.
                if self.scrollbar_paging.is_none() || !self.scrollbar_controller.has_captured_scrollbar() {
                    self.stop_scrollbar_paging();
                    return None;
                }
                // Once the thumb reaches the pointer, paging waits for the pointer to move.
                self.page_captured_scrollbar(now)?;
                self.request_rendering_update();
                self.request_display_ticks_if_needed(now);
                self.host.set_timer(
                    self.context_id,
                    ContextTimer::ScrollbarPaging,
                    SCROLLBAR_PAGING_REPEAT_INTERVAL,
                );
                self.scrollbar_paging_timer_is_armed = true;
            }
            ContextTimer::ScrollbarFadeOut => self.request_display_ticks_if_needed(now),
            ContextTimer::CaretBlink => {
                let damage_rect = self.damage_rect_of_blinking_carets();
                self.schedule_next_caret_blink(now);
                if !damage_rect.is_empty() {
                    return Some(damage_rect);
                }
            }
        }
        None
    }

    pub fn request_rendering_update(&mut self) {
        // The rendering update this asks for adopts the scroll updates it finds pushed by then. What is pending here goes
        // ahead of the request on the wire, so that an update run on the request never misses it.
        if self.has_pending_async_scroll_updates() {
            let updates = self.take_pending_async_scroll_updates();
            self.host.post_to_web_content(
                self.web_content_client_id,
                compositor_web_content_client::messages::AsyncScrollUpdates {
                    context_id: self.context_id,
                    updates,
                },
            );
        }
        self.host.post_to_web_content(
            self.web_content_client_id,
            compositor_web_content_client::messages::RequestRenderingUpdate {},
        );
    }

    pub fn post_clock_tick(&self, frame_time: MonotonicTime, frame_interval_milliseconds: f64) {
        self.host.post_to_render_clock_of_web_content(
            self.web_content_client_id,
            render_clock_client::messages::ClockTick {
                context_id: self.context_id,
                frame_time_nanoseconds: frame_time.nanoseconds(),
                frame_interval_milliseconds,
                scroll_offsets: self.scroll_offsets(),
            },
        );
    }

    pub fn dispatch_mouse_event_to_web_content(
        &self,
        event: &MouseEvent,
        nested_context_scrolled_since_last_frame: bool,
    ) {
        assert!(self.page_id.is_some());
        self.host.post_to_web_content(
            self.web_content_client_id,
            compositor_web_content_client::messages::MouseEvent {
                page_id: self.page_id.unwrap(),
                event: event.clone(),
            },
        );
        // The render clock hears where the pointer went too, without waiting for WebContent's main thread, so that hover
        // can follow the pointer while that thread runs a task. The lanes hit test it in the frames on screen, which a
        // scroll since, here or in a nested context, has moved. A button going up ends the drag a lane follows, whether or
        // not a move comes after it.
        if matches!(event.r#type, MouseEventType::MouseMove | MouseEventType::MouseUp) {
            self.host.post_to_render_clock_of_web_content(
                self.web_content_client_id,
                render_clock_client::messages::PointerMoved {
                    context_id: self.context_id,
                    position: event.position,
                    buttons: event.buttons.0 as u32,
                    scrolled_since_frame: self.scrolled_since_last_frame() || nested_context_scrolled_since_last_frame,
                    input_event_id: event.id,
                },
            );
        } else if event.r#type == MouseEventType::MouseLeave {
            self.host.post_to_render_clock_of_web_content(
                self.web_content_client_id,
                render_clock_client::messages::PointerLeft {
                    context_id: self.context_id,
                    input_event_id: event.id,
                },
            );
        }
    }

    // Whether the scroll offsets here moved past those of the last frame WebContent presented.
    pub fn scrolled_since_last_frame(&self) -> bool {
        self.has_pending_async_scroll_updates()
            || self.scroll_offsets_differ_from_last_frame
            || self.has_active_smooth_scroll_animations()
            || self.has_active_scroll_fling()
    }

    pub fn dispatch_key_event_to_web_content(&self, event: &KeyEvent) {
        assert!(self.page_id.is_some());
        self.host.post_to_web_content(
            self.web_content_client_id,
            compositor_web_content_client::messages::KeyEvent {
                page_id: self.page_id.unwrap(),
                event: event.clone(),
            },
        );
    }

    pub fn dispatch_pinch_event_to_web_content(&self, event: &PinchEvent) {
        assert!(self.page_id.is_some());
        self.host.post_to_web_content(
            self.web_content_client_id,
            compositor_web_content_client::messages::PinchEvent {
                page_id: self.page_id.unwrap(),
                event: event.clone(),
            },
        );
    }

    pub fn stop_presenting_to_client(&mut self) {
        self.pointer_in_nested_contexts = Default::default();
        self.end_keyboard_scroll_gesture();
        self.elastic_overscroll_controller.forget();
        self.presents_to_client = false;
    }

    pub fn set_parent_context(&mut self, parent_context_id: Option<CompositorContextId>) {
        self.parent_context_id = parent_context_id;
    }

    // Returns false, leaving everything as it was, for scrolling state with numbers no scroll can be calculated with.
    pub fn install_display_list_update(
        &mut self,
        display_list: Rc<DisplayList>,
        visual_context_tree: AccumulatedVisualContextTree,
        scroll_state_snapshot: ScrollStateSnapshot,
        now: MonotonicTime,
    ) -> bool {
        assert_eq!(
            display_list.compatible_visual_context_tree_structural_epoch(),
            visual_context_tree.structural_epoch()
        );
        let async_scrolling_state = async_scrolling_state_from(&display_list, visual_context_tree.as_arc());
        if !scroll_state_snapshot.has_only_finite_offsets()
            || !async_scrolling_state.has_only_numbers_scrolls_can_be_calculated_with()
        {
            return false;
        }
        let scroll_offsets_of_known_nodes = self.scroll_offsets_by_stable_id();
        self.scene.install(display_list, visual_context_tree, now);
        self.keyboard_scroll_controller.forget_target();
        self.scroll_state_snapshot = scroll_state_snapshot;
        self.scroll_state_snapshot
            .set_node_count(self.scene.visual_context_tree().as_ref().unwrap().spatial_node_count());
        let display_list = self.scene.display_list().as_ref().unwrap().clone();
        self.restart_caret_blink_for(&display_list, now);
        // The visual viewport of another document is where that document's visual context tree has it.
        if self.scene.has_async_visual_viewport_transform()
            && async_scrolling_state.document_id != self.visual_viewport_document_id
        {
            self.scene.drop_async_visual_viewport_transform();
            self.visual_viewport_document_id = None;
        }
        // NB: What the page of one document let through of a pinch says nothing about the page of the next.
        if async_scrolling_state.document_id != self.async_scroll_tree.document_id() {
            self.pinch_gesture_was_let_through = false;
        }
        self.adopt_scrolling_state(async_scrolling_state, &scroll_offsets_of_known_nodes);
        if let Some(metadata) = self.scene.display_list().as_ref().unwrap().async_scrolling_metadata() {
            self.adopt_keyboard_scroll_state(metadata.keyboard_scroll_state.clone());
        }
        true
    }

    fn adopt_scrolling_state(
        &mut self,
        async_scrolling_state: AsyncScrollingState,
        scroll_offsets_of_known_nodes: &HashMap<AsyncScrollNodeStableID, FloatPoint>,
    ) {
        let mut async_scrolling_viewport_rect = async_scrolling_state.viewport_rect;
        self.wheel_gesture_routing
            .did_install_scrolling_state(&async_scrolling_state);
        let box_of_dragged_scrollbar = self.box_of_captured_scrollbar();
        self.scrollbar_controller
            .set_scrollbars(&async_scrolling_state.scrollbars);
        if !self.scrollbar_controller.has_captured_scrollbar() {
            self.stop_scrollbar_paging();
        }
        self.note_user_scroll_gesture_end_if_drag_ended(box_of_dragged_scrollbar);
        self.async_scroll_tree.set_state(async_scrolling_state);
        self.elastic_overscroll_controller
            .did_install_scrolling_state(&self.async_scroll_tree);
        self.scroll_snap_controller
            .did_install_scrolling_state(&self.async_scroll_tree);
        if let Some(offset) = self.keep_scroll_offsets_of_known_nodes(scroll_offsets_of_known_nodes) {
            async_scrolling_viewport_rect.x = offset.x as i32;
            async_scrolling_viewport_rect.y = offset.y as i32;
        }
        self.rebuild_wheel_hit_test_targets();
        self.async_scrolling_viewport_rect = async_scrolling_viewport_rect;
        self.has_async_scrolling_state = true;
    }

    // Repaints the blinking caret of a display list each time its blink cycle turns it on or off.
    fn restart_caret_blink_for(&mut self, display_list: &DisplayList, now: MonotonicTime) {
        let mut blink_cycle_start_time_ns = None;
        display_list.for_each_caret(|_, _, caret| {
            if caret.should_blink {
                blink_cycle_start_time_ns = Some(caret.blink_cycle_start_time_ns);
            }
        });
        if blink_cycle_start_time_ns == self.caret_blink_cycle_start_time_ns {
            return;
        }
        self.caret_blink_cycle_start_time_ns = blink_cycle_start_time_ns;
        self.host.cancel_timer(self.context_id, ContextTimer::CaretBlink);
        if self.caret_blink_cycle_start_time_ns.is_none() {
            return;
        }
        self.schedule_next_caret_blink(now);
    }

    fn schedule_next_caret_blink(&self, now: MonotonicTime) {
        let Some(start) = self.caret_blink_cycle_start_time_ns else {
            return;
        };
        let now_ns = now.nanoseconds();
        let elapsed_ns = if now_ns > start { now_ns - start } else { 0 };
        let delay_ns = 500_000_000 - elapsed_ns % 500_000_000;
        self.host.set_timer(
            self.context_id,
            ContextTimer::CaretBlink,
            Duration::from_nanoseconds(delay_ns),
        );
    }

    fn damage_rect_of_blinking_carets(&mut self) -> IntRect {
        if self.scene.display_list().is_none() || self.scene.visual_context_tree().is_none() {
            return Default::default();
        }
        let display_list = self.scene.display_list().as_ref().unwrap().clone();
        let visual_context_tree = self.scene.visual_context_tree_for_compositing().clone();
        let stretched_scroll_state_snapshot = self.scroll_state_snapshot_stretched_by_elastic_overscrolls();
        let presented_scroll_state_snapshot = stretched_scroll_state_snapshot
            .as_ref()
            .unwrap_or(&self.scroll_state_snapshot);
        let mut damage_rect = IntRect::default();
        let viewport_rect = IntRect::new(0, 0, self.viewport_size.width, self.viewport_size.height);
        display_list.for_each_caret(|context, bounding_rect, caret| {
            if !caret.should_blink {
                return;
            }
            let mut rect = visual_context_tree.transform_rect_to_viewport(
                context.spatial,
                bounding_rect.to_float(),
                presented_scroll_state_snapshot,
                IncludeVisualViewportTransform::Yes,
            );
            if !rect.x.is_finite() || !rect.y.is_finite() || !rect.width.is_finite() || !rect.height.is_finite() {
                damage_rect = viewport_rect;
                return;
            }
            rect = rect.intersected(viewport_rect.to_float());
            if !rect.is_empty() {
                damage_rect = damage_rect.united(enclosing_int_rect(rect).inflated_edges(1, 1, 1, 1));
            }
        });
        damage_rect = damage_rect.intersected(viewport_rect);
        damage_rect
    }

    // Returns false for a tree that does not fit the installed display list, which keeps the tree it has and takes only
    // the scroll state that pairs with the new one.
    pub fn update_visual_context_tree(
        &mut self,
        visual_context_tree: AccumulatedVisualContextTree,
        scroll_state_snapshot: ScrollStateSnapshot,
        keyboard_scroll_state: KeyboardScrollState,
        now: MonotonicTime,
    ) -> bool {
        let scroll_offsets_of_known_nodes = self.scroll_offsets_by_stable_id();
        let previous_visual_context_tree = self.scene.visual_context_tree().clone();
        if !self.scene.replace_visual_context_tree(visual_context_tree, now) {
            self.update_scroll_state(scroll_state_snapshot, keyboard_scroll_state);
            return false;
        }
        let compositor_scroll_table_changed = previous_visual_context_tree.is_some_and(|previous| {
            previous.as_arc().compositor_scroll_table()
                != self
                    .scene
                    .visual_context_tree()
                    .as_ref()
                    .unwrap()
                    .as_arc()
                    .compositor_scroll_table()
        });
        self.update_scroll_state_with_known_offsets(
            scroll_state_snapshot,
            keyboard_scroll_state,
            &scroll_offsets_of_known_nodes,
            compositor_scroll_table_changed,
        );
        true
    }

    pub fn update_scroll_state(
        &mut self,
        scroll_state_snapshot: ScrollStateSnapshot,
        keyboard_scroll_state: KeyboardScrollState,
    ) {
        let scroll_offsets_of_known_nodes = self.scroll_offsets_by_stable_id();
        self.update_scroll_state_with_known_offsets(
            scroll_state_snapshot,
            keyboard_scroll_state,
            &scroll_offsets_of_known_nodes,
            false,
        );
    }

    fn update_scroll_state_with_known_offsets(
        &mut self,
        scroll_state_snapshot: ScrollStateSnapshot,
        keyboard_scroll_state: KeyboardScrollState,
        scroll_offsets_of_known_nodes: &HashMap<AsyncScrollNodeStableID, FloatPoint>,
        compositor_scroll_table_changed: bool,
    ) {
        self.scene.forget_whether_animated_content_may_affect_viewport();
        // Newly adopted stable ids must start at the offsets in the snapshot that pairs with this tree.
        self.scroll_state_snapshot = scroll_state_snapshot;
        self.scroll_state_snapshot.set_node_count(
            self.scene
                .visual_context_tree()
                .as_ref()
                .map_or(0, |tree| tree.spatial_node_count()),
        );
        if !self.has_async_scrolling_state {
            return;
        }
        if compositor_scroll_table_changed {
            let async_scrolling_state = async_scrolling_state_from(
                self.scene.display_list().as_ref().unwrap(),
                self.scene.visual_context_tree().as_ref().unwrap().as_arc(),
            );
            self.adopt_scrolling_state(async_scrolling_state, scroll_offsets_of_known_nodes);
        } else {
            let kept_viewport_scroll_offset = self.keep_scroll_offsets_of_known_nodes(scroll_offsets_of_known_nodes);
            self.rebuild_wheel_hit_test_targets();
            if let Some(offset) = kept_viewport_scroll_offset {
                self.async_scrolling_viewport_rect.x = offset.x as i32;
                self.async_scrolling_viewport_rect.y = offset.y as i32;
            }
        }
        self.adopt_keyboard_scroll_state(keyboard_scroll_state);
    }

    pub fn invalidate_wheel_event_listener_state(&mut self, generation: u64) {
        self.wheel_gesture_routing
            .invalidate_wheel_event_listener_state(generation);
    }
    pub fn invalidate_keyboard_scroll_state(&mut self, generation: u64) {
        self.keyboard_scroll_controller.invalidate_state(generation);
    }

    fn adopt_keyboard_scroll_state(&mut self, state: KeyboardScrollState) {
        if self.keyboard_scroll_controller.adopt_state(state) && !self.keyboard_scroll_controller.has_target() {
            self.end_keyboard_scroll_gesture();
        }
    }

    fn end_keyboard_scroll_gesture(&mut self) {
        if !self.keyboard_scroll_controller.release_all_keys() {
            return;
        }
        self.end_user_scroll_gesture_of_held_keys();
        self.request_rendering_update();
    }
}

fn clamp<T: PartialOrd>(value: T, min: T, max: T) -> T {
    if value > max {
        max
    } else if value < min {
        min
    } else {
        value
    }
}

fn max<T: PartialOrd>(a: T, b: T) -> T {
    if a < b { b } else { a }
}
