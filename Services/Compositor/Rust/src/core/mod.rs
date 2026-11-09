/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Context lifecycle, frame scheduling, and input routing for the compositor.

mod input;
pub mod messages;
mod testing;
mod wheel;

use crate::context_state::{
    ContextState, ContextUpdateResult, FirstWheelStepRoute, ScreenshotInputs, ScrollCapabilities,
};
use crate::frame_inputs::FrameInputs;
use crate::host::{
    ContextTimer, DisplayTickInputs, DisplayTickResult, Host, PendingFrame, PresentRequest, PresentScheduling,
    PresentationId, WebContentClientId,
};
use crate::wheel_gesture_routing::WheelGestureIdAllocator;
use libcompositing_rust::ipc_types::Compositing_ContextVisibility as ContextVisibility;
use libcompositing_rust::scrolling::{
    AccumulatedVisualContextTree, CompositorContextId, DisplayList, KeyboardScrollState, ScrollStateSnapshot,
};
use libcompositing_rust::time::MonotonicTime;
use libgfx_rust::{FloatPoint, IntSize};
use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::rc::Rc;

pub struct CompositorCore {
    host: Rc<Host>,
    contexts: HashMap<CompositorContextId, RefCell<ContextState>>,
    present_requests: RefCell<Vec<PresentRequest>>,
    is_entered: Cell<bool>,
    wheel_gesture_id_allocator: Rc<WheelGestureIdAllocator>,
    scrollbars_always_shown: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum ContextOwnerCheckResult {
    OwnedByClient,
    ContextUnavailable,
    ConflictingOwner,
}

// What became of a visual context tree that came with the scroll state pairing with it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum VisualContextTreeUpdateResult {
    TreeReplaced,
    // The tree does not fit the installed display list, so only its scroll state was taken.
    TreeDropped,
    InvalidScrollState,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum WheelStepOrigin {
    UI,
    Fling,
    MainThread,
}

// How a wheel step went in a context that presented and published what the step changed itself.
#[derive(Default)]
struct AppliedWheelStep {
    // What is left for the context that routes the step to present and report.
    result: ContextUpdateResult,
    presents_frame: bool,
}

struct NestedContextTakingWheelStep<'a> {
    context_id: CompositorContextId,
    // Null when the step cannot be handed to the context.
    context: Option<&'a RefCell<ContextState>>,
    position: FloatPoint,
    route: FirstWheelStepRoute,
}

struct FirstWheelStepInNestedContexts<'a> {
    // The innermost context that the step went into, whose page sees the step.
    innermost_context_id: CompositorContextId,
    // The context that takes the step, which is empty when each of them passes the step on.
    taking_context: Option<NestedContextTakingWheelStep<'a>>,
}

struct Entered<'a>(&'a Cell<bool>);

impl<'a> Entered<'a> {
    fn new(is_entered: &'a Cell<bool>) -> Self {
        assert!(!is_entered.get());
        is_entered.set(true);
        Self(is_entered)
    }
}

impl Drop for Entered<'_> {
    fn drop(&mut self) {
        self.0.set(false);
    }
}

impl CompositorCore {
    pub fn new(host: Rc<Host>, scrollbars_always_shown: bool) -> Self {
        Self {
            host,
            contexts: HashMap::new(),
            present_requests: RefCell::new(Vec::new()),
            is_entered: Cell::new(false),
            wheel_gesture_id_allocator: Rc::new(WheelGestureIdAllocator::default()),
            scrollbars_always_shown,
        }
    }

    pub fn create_context(
        &mut self,
        context_id: CompositorContextId,
        page_id: Option<u64>,
        web_content_client_id: WebContentClientId,
    ) {
        let _entered = Entered::new(&self.is_entered);

        assert!(!self.contexts.contains_key(&context_id));
        if let Some(page_id) = page_id {
            assert!(page_id > 0);
            assert_eq!(context_id.0, page_id);
        }

        self.contexts.entry(context_id).or_insert_with(|| {
            RefCell::new(ContextState::new(
                context_id,
                page_id,
                web_content_client_id,
                self.host.clone(),
                self.wheel_gesture_id_allocator.clone(),
                self.scrollbars_always_shown,
            ))
        });
    }

    // Also leaves the contexts nested in it without a parent.
    pub fn destroy_context(&mut self, context_id: CompositorContextId) {
        let _entered = Entered::new(&self.is_entered);

        assert!(self.context_if_present(context_id).is_some());
        for (&id, possible_child_context) in &self.contexts {
            if id == context_id {
                continue;
            }
            let parent_context_id = possible_child_context.borrow().parent_context_id();
            if parent_context_id == Some(context_id) {
                possible_child_context.borrow_mut().set_parent_context(None);
            }
        }
        self.contexts.remove(&context_id);
    }

    pub fn check_context_owner(
        &self,
        context_id: CompositorContextId,
        client: WebContentClientId,
    ) -> ContextOwnerCheckResult {
        let _entered = self.enter();

        let Some(context) = self.context_if_present(context_id) else {
            return ContextOwnerCheckResult::ContextUnavailable;
        };
        if !context.borrow().is_owned_by(client) {
            return ContextOwnerCheckResult::ConflictingOwner;
        }

        ContextOwnerCheckResult::OwnedByClient
    }

    pub fn set_parent_context(&self, context_id: CompositorContextId, parent_context_id: Option<CompositorContextId>) {
        let _entered = self.enter();

        let context = self.context_if_present(context_id).expect("context exists");
        context.borrow_mut().set_parent_context(parent_context_id);
    }

    pub fn stop_presenting_to_client(&self, context_id: CompositorContextId) {
        let _entered = self.enter();

        let context = self.context_if_present(context_id).expect("context exists");
        context.borrow_mut().stop_presenting_to_client();
    }

    pub fn viewport_size_updated(&self, context_id: CompositorContextId, viewport_size: IntSize) {
        let _entered = self.enter();

        let Some(context) = self.context_if_present(context_id) else {
            return;
        };
        context.borrow_mut().viewport_size_updated(viewport_size);
    }

    pub fn set_paused_debugger_overlay_is_shown(&self, context_id: CompositorContextId, is_shown: bool) {
        let _entered = self.enter();

        let Some(context) = self.context_if_present(context_id) else {
            return;
        };
        context.borrow_mut().set_paused_debugger_overlay_is_shown(is_shown);
    }

    pub fn set_bound_presentation(&self, context_id: CompositorContextId, presentation_id: Option<PresentationId>) {
        let _entered = self.enter();

        let context = self.context_if_present(context_id).expect("context exists");
        context.borrow_mut().set_bound_presentation(presentation_id);
    }

    pub fn set_scroll_capabilities(&self, context_id: CompositorContextId, capabilities: &ScrollCapabilities) {
        let _entered = self.enter();

        let context = self.context_if_present(context_id).expect("context exists");
        context.borrow_mut().set_scroll_capabilities(capabilities);
    }

    // Returns false, leaving everything as it was, for scrolling state with numbers no scroll can be calculated with.
    pub fn install_display_list_update(
        &self,
        context_id: CompositorContextId,
        display_list: Rc<DisplayList>,
        visual_context_tree: AccumulatedVisualContextTree,
        scroll_state_snapshot: ScrollStateSnapshot,
        now: MonotonicTime,
    ) -> bool {
        let _entered = self.enter();

        let context = self.context_if_present(context_id).expect("context exists");
        context
            .borrow_mut()
            .install_display_list_update(display_list, visual_context_tree, scroll_state_snapshot, now)
    }

    // Returns false for a tree that does not fit the installed display list, of which only the scroll state is taken.
    pub fn update_visual_context_tree(
        &self,
        context_id: CompositorContextId,
        visual_context_tree: AccumulatedVisualContextTree,
        scroll_state_snapshot: ScrollStateSnapshot,
        keyboard_scroll_state: KeyboardScrollState,
        now: MonotonicTime,
    ) -> bool {
        let _entered = self.enter();

        let context = self.context_if_present(context_id).expect("context exists");
        context.borrow_mut().update_visual_context_tree(
            visual_context_tree,
            scroll_state_snapshot,
            keyboard_scroll_state,
            now,
        )
    }

    pub fn needs_animation_frames(&self, context_id: CompositorContextId, now: MonotonicTime) -> bool {
        let _entered = self.enter();

        let context = self.context_if_present(context_id).expect("context exists");
        context.borrow_mut().needs_animation_frames(now)
    }

    pub fn frame_inputs(&self, context_id: CompositorContextId, now: MonotonicTime) -> Option<FrameInputs> {
        let _entered = self.enter();

        let context = self.context_if_present(context_id).expect("context exists");
        context.borrow_mut().frame_inputs(now)
    }

    pub fn frame_inputs_for_screenshot(&self, context_id: CompositorContextId, now: MonotonicTime) -> ScreenshotInputs {
        let _entered = self.enter();

        let context = self.context_if_present(context_id).expect("context exists");
        context.borrow_mut().frame_inputs_for_screenshot(now)
    }

    // Ends the fling of a context that is hidden, whether or not it was visible.
    pub fn set_visibility(&self, context_id: CompositorContextId, visibility: ContextVisibility, now: MonotonicTime) {
        let _entered = self.enter();

        let Some(context) = self.context_if_present(context_id) else {
            return;
        };
        if visibility == ContextVisibility::Hidden {
            let event = context.borrow_mut().end_scroll_fling();
            self.dispatch_scroll_fling_step(context_id, context, event, now);
        }
        context.borrow_mut().set_visibility(visibility);
    }

    pub fn timer_fired(&self, context_id: CompositorContextId, timer: ContextTimer, now: MonotonicTime) {
        let _entered = self.enter();

        let Some(context) = self.context_if_present(context_id) else {
            return;
        };
        let caret_damage_rect = context.borrow_mut().timer_fired(timer, now);
        if let Some(caret_damage_rect) = caret_damage_rect {
            self.present_requests.borrow_mut().push(PresentRequest {
                context_id,
                viewport_rect: None,
                forced_damage_rect: caret_damage_rect,
                scheduling: PresentScheduling::OnNextDisplayTick,
            });
        }
    }

    // The frames asked for since the last call, in the order they were asked for.
    pub fn take_present_requests(&self) -> Vec<PresentRequest> {
        let _entered = self.enter();

        std::mem::take(&mut *self.present_requests.borrow_mut())
    }

    pub(super) fn context_still_nested_in(
        &self,
        containing_context_id: CompositorContextId,
        context_id: Option<CompositorContextId>,
    ) -> Option<&RefCell<ContextState>> {
        let context_id = context_id?;
        let context = self.context_if_present(context_id)?;
        if !self.is_nested_in(containing_context_id, context) {
            return None;
        }
        Some(context)
    }

    // Whether a context nested in the context `context_id` scrolled since the last frame its WebContent presented: the lane
    // of the context's document hands a pointer move to the lane of a same-process iframe, which hit tests it in the iframe.
    pub(super) fn nested_context_scrolled_since_last_frame(&self, context_id: CompositorContextId) -> bool {
        for context in self.contexts.values() {
            if context.borrow().scrolled_since_last_frame() && self.is_nested_in(context_id, context) {
                return true;
            }
        }
        false
    }

    pub(super) fn is_nested_in(
        &self,
        containing_context_id: CompositorContextId,
        context: &RefCell<ContextState>,
    ) -> bool {
        // A walk up the parents that outlasts the number of contexts went around a loop.
        let mut ancestor = Some(context);
        for _ in 0..self.contexts.len() {
            let Some(current_ancestor) = ancestor else { break };
            let Some(parent_context_id) = current_ancestor.borrow().parent_context_id() else {
                return false;
            };
            if parent_context_id == containing_context_id {
                return true;
            }
            ancestor = self.context_if_present(parent_context_id);
        }
        false
    }

    // Hands the context's async scroll updates to its WebContent process as soon as they exist,
    // so a rendering update reads them locally instead of asking for them over a synchronous call.
    pub(super) fn publish_pending_async_scroll_updates(&self, _: CompositorContextId, context: &RefCell<ContextState>) {
        context.borrow_mut().publish_pending_async_scroll_updates();
    }

    pub(super) fn request_present(&self, context_id: CompositorContextId, frame: &PendingFrame) {
        self.present_requests.borrow_mut().push(PresentRequest {
            context_id,
            viewport_rect: Some(frame.viewport_rect),
            forced_damage_rect: frame.forced_damage_rect,
            scheduling: PresentScheduling::OnNextDisplayTick,
        });
    }

    pub fn display_tick(
        &self,
        context_id: CompositorContextId,
        frame_time: MonotonicTime,
        now: MonotonicTime,
        inputs: &DisplayTickInputs,
    ) -> DisplayTickResult {
        let _entered = self.enter();

        let context = self.context_if_present(context_id).expect("context exists");

        let has_active_animation_on_display = (context.borrow_mut().needs_animation_frames(now)
            || inputs.ui_overlay_fades_need_frame)
            && inputs.context_is_on_tick_display;
        let presents_on_display = inputs.present_is_scheduled_on_tick_display || has_active_animation_on_display;
        if presents_on_display {
            if inputs.context_is_on_tick_display {
                let event = context.borrow_mut().take_scroll_fling_step(frame_time);
                self.dispatch_scroll_fling_step(context_id, context, event, frame_time);
            }
            let animation_frame = context.borrow_mut().advance_smooth_scroll_animations(frame_time);
            if let Some(animation_frame) = animation_frame {
                self.present_requests.borrow_mut().push(PresentRequest {
                    context_id,
                    viewport_rect: Some(animation_frame),
                    forced_damage_rect: Default::default(),
                    scheduling: PresentScheduling::WithDisplayTickInProgress,
                });
            }
            let rebound_frame = context.borrow_mut().advance_elastic_overscroll_rebounds(frame_time);
            if let Some(rebound_frame) = rebound_frame {
                self.present_requests.borrow_mut().push(PresentRequest {
                    context_id,
                    viewport_rect: Some(rebound_frame),
                    forced_damage_rect: Default::default(),
                    scheduling: PresentScheduling::WithDisplayTickInProgress,
                });
            }
            let damage_rect = context
                .borrow_mut()
                .advance_scrollbar_fades_and_return_damage_rect(frame_time);
            if !damage_rect.is_empty() {
                self.present_requests.borrow_mut().push(PresentRequest {
                    context_id,
                    viewport_rect: None,
                    forced_damage_rect: damage_rect,
                    scheduling: PresentScheduling::WithDisplayTickInProgress,
                });
            }
        }

        // A render clock tick goes to the process's render clock thread, beside what its main thread is doing, with
        // where the compositor has scrolled to for this vsync, which that main thread may not have taken in yet.
        if let Some(interval) = inputs.clock_tick_interval_in_milliseconds {
            context.borrow().post_clock_tick(frame_time, interval);
        }

        if presents_on_display {
            self.publish_pending_async_scroll_updates(context_id, context);
            let needs_frame = context.borrow_mut().visual_animations_need_frame(now);
            if needs_frame {
                context.borrow_mut().advance_visual_animations(frame_time);
                self.present_requests.borrow_mut().push(PresentRequest {
                    context_id,
                    viewport_rect: None,
                    forced_damage_rect: Default::default(),
                    scheduling: PresentScheduling::WithDisplayTickInProgress,
                });
            }
        }
        DisplayTickResult {
            presents_on_display,
            needs_animation_frames: context.borrow_mut().needs_animation_frames(now),
        }
    }

    pub(super) fn context_if_present(&self, context_id: CompositorContextId) -> Option<&RefCell<ContextState>> {
        self.contexts.get(&context_id)
    }

    pub(super) fn apply_context_update_result(
        &self,
        context_id: CompositorContextId,
        context: &RefCell<ContextState>,
        result: &ContextUpdateResult,
    ) -> bool {
        if let Some(frame) = result.frame_to_present {
            self.request_present(context_id, &frame);
        }
        self.publish_pending_async_scroll_updates(context_id, context);
        if result.should_request_rendering_update {
            context.borrow_mut().request_rendering_update();
        }
        result.accepted
    }

    fn enter(&self) -> Entered<'_> {
        Entered::new(&self.is_entered)
    }
}
