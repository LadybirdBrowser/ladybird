/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Context lifecycle, frame scheduling, and input routing for the compositor.

mod input;
pub mod messages;
pub mod presenting;
mod testing;
mod wheel;

use crate::context_state::{ContextState, ContextUpdateResult, FirstWheelStepRoute};
use crate::host::{
    ContextTimer, DisplayTickInputs, DisplayTickResult, Host, PendingFrame, PresentRequest, PresentScheduling,
    PresentationId, WebContentClientId,
};
use crate::overlays::UIOverlays;
use crate::presenting::ContextPresenting;
use crate::raster_host::{RasterHost, SurfaceHandle};
use crate::wheel_gesture_routing::WheelGestureIdAllocator;
use libcompositing_rust::ipc_types::Compositing_ContextVisibility as ContextVisibility;
use libcompositing_rust::scrolling::{
    AccumulatedVisualContextTree, CompositorContextId, DisplayList, KeyboardScrollState, ScrollStateSnapshot,
};
use libcompositing_rust::time::MonotonicTime;
use libgfx_rust::{FloatPoint, IntSize};
use presenting::{GpuBackend, PendingAsyncPresent, PresentationCapabilities};
use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::rc::Rc;

pub struct CompositorCore {
    host: Rc<Host>,
    raster: RasterHost,
    gpu: GpuBackend,
    contexts: HashMap<CompositorContextId, RefCell<ContextState>>,
    presenting: HashMap<CompositorContextId, RefCell<ContextPresenting>>,
    presentations: RefCell<HashMap<PresentationId, PresentationCapabilities>>,
    async_presents: RefCell<Vec<PendingAsyncPresent>>,
    window_server_may_show_high_dynamic_range: Cell<bool>,
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
    pub fn new(host: Rc<Host>, raster: RasterHost, scrollbars_always_shown: bool, gpu: GpuBackend) -> Self {
        Self {
            host,
            raster,
            gpu,
            contexts: HashMap::new(),
            presenting: HashMap::new(),
            presentations: RefCell::new(HashMap::new()),
            async_presents: RefCell::new(Vec::new()),
            window_server_may_show_high_dynamic_range: Cell::new(true),
            present_requests: RefCell::new(Vec::new()),
            is_entered: Cell::new(false),
            wheel_gesture_id_allocator: Rc::new(WheelGestureIdAllocator::default()),
            scrollbars_always_shown,
        }
    }

    pub fn raster(&self) -> &RasterHost {
        &self.raster
    }

    /// Schedules what a message the core took asked for.
    pub fn finish_message(&self, now: MonotonicTime) {
        let _entered = self.enter();

        self.finish_entry(now);
    }

    pub fn create_context(
        &mut self,
        context_id: CompositorContextId,
        page_id: Option<u64>,
        web_content_client_id: WebContentClientId,
        now: MonotonicTime,
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
        self.presenting
            .insert(context_id, RefCell::new(ContextPresenting::new(page_id.is_some())));
        self.resize_backing_stores_if_needed(context_id, now);
        self.finish_entry(now);
    }

    // Also leaves the contexts nested in it without a parent.
    pub fn destroy_context(&mut self, context_id: CompositorContextId, now: MonotonicTime) {
        let _entered = Entered::new(&self.is_entered);

        assert!(self.context_if_present(context_id).is_some());
        self.cancel_pending_async_presents_for_context(context_id);
        self.clear_parent_context(context_id, now);
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
        self.presenting.remove(&context_id);
        self.update_scroll_capabilities();
        self.raster
            .set_maximum_display_refresh_rate(self.maximum_display_refresh_rate());
        self.finish_entry(now);
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

    // What a client shows is rooted at the context presented to it, and no context is nested in itself.
    pub fn context_can_be_nested_in(
        &self,
        context_id: CompositorContextId,
        parent_context_id: CompositorContextId,
    ) -> bool {
        let _entered = self.enter();

        let context = self.context_if_present(context_id).expect("context exists");
        !context.borrow().presents_to_client() && parent_context_id != context_id
    }

    pub fn presents_to_client(&self, context_id: CompositorContextId) -> bool {
        let _entered = self.enter();

        self.context_if_present(context_id)
            .is_some_and(|context| context.borrow().presents_to_client())
    }

    pub fn context_is_effectively_visible(&self, context_id: CompositorContextId) -> bool {
        let _entered = self.enter();

        self.context_if_present(context_id).is_some() && self.context_is_effectively_visible_inner(context_id)
    }

    pub fn parent_context(&self, context_id: CompositorContextId) -> Option<CompositorContextId> {
        let _entered = self.enter();

        self.context_if_present(context_id)
            .and_then(|context| context.borrow().parent_context_id())
    }

    pub fn bound_presentation(&self, context_id: CompositorContextId) -> Option<PresentationId> {
        let _entered = self.enter();

        self.context_if_present(context_id)
            .and_then(|context| context.borrow().bound_presentation())
    }

    pub fn set_parent_context(
        &self,
        context_id: CompositorContextId,
        parent_context_id: Option<CompositorContextId>,
        now: MonotonicTime,
    ) {
        let _entered = self.enter();

        self.set_parent_context_inner(context_id, parent_context_id, now);
        self.finish_entry(now);
    }

    pub fn stop_presenting_to_client(&self, context_id: CompositorContextId, now: MonotonicTime) {
        let _entered = self.enter();

        let context = self.context_if_present(context_id).expect("context exists");
        self.unbind_presentation_inner(context_id, now);
        context.borrow_mut().stop_presenting_to_client();
        if let Some(presenting) = self.presenting_of(context_id) {
            presenting.borrow_mut().presents_to_client = false;
        }
        self.finish_entry(now);
    }

    pub fn viewport_size_updated(
        &self,
        context_id: CompositorContextId,
        viewport_size: IntSize,
        device_pixel_ratio: f64,
        window_resize_in_progress: bool,
        now: MonotonicTime,
    ) {
        let _entered = self.enter();

        let (Some(context), Some(presenting)) = (self.context_if_present(context_id), self.presenting_of(context_id))
        else {
            return;
        };
        let overlays_repaint_everything = context
            .borrow_mut()
            .viewport_size_updated(viewport_size, device_pixel_ratio);
        let is_page_presentation_context = {
            let context = context.borrow();
            context.page_id().is_some() && context.parent_context_id().is_none()
        };
        presenting.borrow_mut().set_viewport_size(
            viewport_size,
            device_pixel_ratio,
            is_page_presentation_context && window_resize_in_progress,
        );
        self.resize_backing_stores_if_needed(context_id, now);
        if overlays_repaint_everything {
            self.request_present_repainting_everything(context_id, context);
        }
        if presenting.borrow().window_resize_in_progress() {
            presenting
                .borrow()
                .schedule_backing_store_shrink(context_id, &self.host);
        }
        self.finish_entry(now);
    }

    pub fn set_ui_overlays(&self, context_id: CompositorContextId, ui_overlays: UIOverlays, now: MonotonicTime) {
        let _entered = self.enter();

        let Some(context) = self.context_if_present(context_id) else {
            return;
        };
        if context.borrow_mut().set_ui_overlays(ui_overlays, now) {
            self.request_present_repainting_everything(context_id, context);
        }
        self.finish_entry(now);
    }

    // A frame at the viewport rect the context is presented at, which repaints all of it.
    fn request_present_repainting_everything(&self, context_id: CompositorContextId, context: &RefCell<ContextState>) {
        self.present_requests.borrow_mut().push(PresentRequest {
            context_id,
            viewport_rect: None,
            forced_damage_rect: context.borrow().viewport_rect(),
            scheduling: PresentScheduling::OnNextDisplayTick,
        });
    }

    pub fn register_presentation(&self, presentation_id: PresentationId, capabilities: PresentationCapabilities) {
        let _entered = self.enter();

        self.presentations.borrow_mut().insert(presentation_id, capabilities);
    }

    // Leaves every context bound to the presentation unbound.
    pub fn unregister_presentation(&self, presentation_id: PresentationId, now: MonotonicTime) {
        let _entered = self.enter();

        let bound_context_ids: Vec<CompositorContextId> = self
            .contexts
            .iter()
            .filter(|(_, context)| context.borrow().bound_presentation() == Some(presentation_id))
            .map(|(&context_id, _)| context_id)
            .collect();
        for context_id in bound_context_ids {
            self.unbind_presentation_inner(context_id, now);
        }
        self.presentations.borrow_mut().remove(&presentation_id);
        self.finish_entry(now);
    }

    pub fn set_presentation_gpu_capability(
        &self,
        presentation_id: PresentationId,
        supported: bool,
        adapter_luid: u64,
        now: MonotonicTime,
    ) {
        let _entered = self.enter();

        {
            let mut presentations = self.presentations.borrow_mut();
            let Some(capabilities) = presentations.get_mut(&presentation_id) else {
                return;
            };
            let gpu_presentation_adapter_luid = supported.then_some(adapter_luid);
            if capabilities.gpu_presentation_adapter_luid == gpu_presentation_adapter_luid
                && capabilities.cannot_present_gpu_images == !supported
            {
                return;
            }
            capabilities.gpu_presentation_adapter_luid = gpu_presentation_adapter_luid;
            capabilities.cannot_present_gpu_images = !supported;
        }
        // Reallocate the backing stores of the presentation's contexts so they match the new capability.
        let bound_context_ids: Vec<CompositorContextId> = self
            .contexts
            .iter()
            .filter(|(_, context)| context.borrow().bound_presentation() == Some(presentation_id))
            .map(|(&context_id, _)| context_id)
            .collect();
        for context_id in bound_context_ids {
            self.invalidate_and_resize_backing_stores(context_id, now);
        }
        self.finish_entry(now);
    }

    pub fn set_scrollbar_track_press_jumps(&self, presentation_id: PresentationId, jumps: bool) {
        let _entered = self.enter();

        if let Some(capabilities) = self.presentations.borrow_mut().get_mut(&presentation_id) {
            capabilities.scrollbar_track_press_jumps = jumps;
        }
        self.update_scroll_capabilities();
    }

    fn invalidate_and_resize_backing_stores(&self, context_id: CompositorContextId, now: MonotonicTime) {
        if let Some(presenting) = self.presenting_of(context_id) {
            let environment = self.environment(
                context_id,
                now,
                &libcompositing_rust::display_list::replay::NoCompositedContexts,
            );
            presenting.borrow_mut().invalidate_backing_stores(&environment);
        }
        self.resize_backing_stores_if_needed(context_id, now);
    }

    // Each root context presents to the presentation it is bound to. Unbound contexts keep rendering, but publish
    // nothing.
    pub fn bind_presentation(
        &self,
        context_id: CompositorContextId,
        presentation_id: PresentationId,
        now: MonotonicTime,
    ) {
        let _entered = self.enter();

        let Some(context) = self.context_if_present(context_id) else {
            return;
        };
        if !context.borrow().presents_to_client() {
            use std::io::Write;
            let _ = writeln!(
                std::io::stderr(),
                "Compositor: Ignoring presentation binding for context {}, which is not a root context",
                context_id.0
            );
            return;
        }
        if context.borrow().bound_presentation() == Some(presentation_id) {
            return;
        }
        self.unbind_presentation_inner(context_id, now);
        context.borrow_mut().set_bound_presentation(Some(presentation_id));
        self.update_scroll_capabilities();
        // The new presentation has seen none of the context's buffers, so it gets buffers of its own.
        self.invalidate_and_resize_backing_stores(context_id, now);
        self.finish_entry(now);
    }

    pub fn set_shown_in_remote_layer(&self, context_id: CompositorContextId, shown: bool, now: MonotonicTime) {
        let _entered = self.enter();

        if shown {
            self.start_showing_context_in_remote_layer(context_id, now);
        } else {
            self.stop_showing_context_in_remote_layer(context_id, now);
        }
        self.finish_entry(now);
    }

    pub fn set_display_metadata(
        &self,
        context_id: CompositorContextId,
        display_id: Option<u64>,
        refresh_rate: f64,
        now: MonotonicTime,
    ) {
        let _entered = self.enter();

        assert!(refresh_rate.is_finite() && refresh_rate > 0.0);
        let Some(presenting) = self.presenting_of(context_id) else {
            return;
        };
        let (pending_present_frame_scheduled, wants_ticks) = {
            let mut presenting = presenting.borrow_mut();
            presenting.display_id = display_id;
            presenting.display_refresh_rate = refresh_rate;
            (
                presenting.pending_present_frame_scheduled(),
                presenting.rendering_opportunity_request.is_requested() || presenting.clock_tick_request.is_requested(),
            )
        };
        if pending_present_frame_scheduled {
            self.schedule_pending_present_frame(context_id, now);
        }
        self.raster
            .set_maximum_display_refresh_rate(self.maximum_display_refresh_rate());
        if wants_ticks && self.context_is_effectively_visible_inner(context_id) {
            let (display_id, refresh_rate) = self.display_of(context_id);
            self.raster.request_vsync(display_id, refresh_rate);
        }
        self.finish_entry(now);
    }

    pub fn set_window_server_may_show_high_dynamic_range(&self, may_show: bool, now: MonotonicTime) {
        let _entered = self.enter();

        if self.window_server_may_show_high_dynamic_range.replace(may_show) == may_show {
            return;
        }
        let context_ids: Vec<CompositorContextId> = self.presenting.keys().copied().collect();
        for context_id in context_ids {
            self.present_current_frame(context_id, now);
        }
        self.finish_entry(now);
    }

    pub fn set_presents_from_cpu_memory(
        &self,
        context_id: CompositorContextId,
        presents_from_cpu_memory: bool,
        now: MonotonicTime,
    ) {
        let _entered = self.enter();

        let Some(presenting) = self.presenting_of(context_id) else {
            return;
        };
        if presenting.borrow().presents_from_cpu_memory == presents_from_cpu_memory {
            return;
        }
        let previous_gpu_sharing = self.gpu_sharing_allowed(context_id);
        presenting.borrow_mut().presents_from_cpu_memory = presents_from_cpu_memory;
        // NB: Without a GPU, the backing stores are in CPU memory either way.
        if !self.gpu.has_gpu || self.gpu_sharing_allowed(context_id) == previous_gpu_sharing {
            return;
        }
        // NB: Otherwise, the presentation would read every frame back from GPU memory on its main thread.
        self.invalidate_and_resize_backing_stores(context_id, now);
        self.finish_entry(now);
    }

    pub fn request_rendering_opportunity(
        &self,
        context_id: CompositorContextId,
        maximum_frames_per_second: f64,
        now: MonotonicTime,
    ) {
        let _entered = self.enter();

        self.request_rendering_opportunity_inner(context_id, maximum_frames_per_second, now);
    }

    // Asks for the next display tick the context's maximum rate lets through, for its render clock.
    pub fn request_clock_tick(&self, context_id: CompositorContextId, maximum_frames_per_second: f64) {
        let _entered = self.enter();

        let Some(presenting) = self.presenting_of(context_id) else {
            return;
        };
        if !presenting
            .borrow_mut()
            .clock_tick_request
            .request(maximum_frames_per_second)
            || !self.context_is_effectively_visible_inner(context_id)
        {
            return;
        }
        let (display_id, refresh_rate) = self.display_of(context_id);
        self.raster.request_vsync(display_id, refresh_rate);
    }

    // Delivers the rendering opportunity a context requested now rather than at the next display tick: a viewport
    // change that arrived while an animation's opportunity was outstanding starts its update at once.
    pub fn hurry_rendering_opportunity(&self, context_id: CompositorContextId, now: MonotonicTime) {
        let _entered = self.enter();

        self.hurry_rendering_opportunity_inner(context_id, now);
    }

    // A frame the page laid out at the viewport rect.
    pub fn present_frame_of_page(
        &self,
        context_id: CompositorContextId,
        viewport_rect: libgfx_rust::IntRect,
        now: MonotonicTime,
    ) {
        let _entered = self.enter();

        let Some(presenting) = self.presenting_of(context_id) else {
            return;
        };
        {
            let mut presenting = presenting.borrow_mut();
            presenting.size_laid_out_by_page = IntSize {
                width: viewport_rect.width,
                height: viewport_rect.height,
            };
            // The frame joins whatever is queued, so a frame queued earlier cannot be presented after it.
            presenting.queue_present_frame(PendingFrame::repainting_changes(viewport_rect));
        }
        if !self.try_present_frame_during_resize(context_id, now) {
            self.schedule_pending_present_frame(context_id, now);
        }
        self.finish_entry(now);
    }

    pub fn presented_store_released(&self, context_id: CompositorContextId, bitmap_id: i32, now: MonotonicTime) {
        let _entered = self.enter();

        if self.presenting_of(context_id).is_none() {
            return;
        }
        if !self.raster.release_store(context_id, bitmap_id) {
            return;
        }
        self.schedule_pending_present_frame_if_unblocked(context_id, now);
        self.finish_entry(now);
    }

    pub fn vsync(&self, display_id: Option<u64>, frame_time: MonotonicTime, now: MonotonicTime) {
        let _entered = self.enter();

        self.vsync_inner(display_id, frame_time, now);
        self.finish_entry(now);
    }

    pub fn gpu_work_finished(&self, token: u64, now: MonotonicTime) {
        let _entered = self.enter();

        self.gpu_work_finished_inner(token, now);
        self.finish_entry(now);
    }

    pub fn frame_ready_for_hand_off(&self, token: u64, now: MonotonicTime) {
        let _entered = self.enter();

        self.frame_ready_for_hand_off_inner(token, now);
        self.finish_entry(now);
    }

    // A video sink or an image frame that the context draws has new pixels.
    pub fn context_content_changed(&self, context_id: CompositorContextId, now: MonotonicTime) {
        let _entered = self.enter();

        self.present_changed_live_content(context_id, now);
        self.finish_entry(now);
    }

    // A canvas of the client has new pixels, which the contexts that drew it last show next.
    pub fn canvas_content_changed(&self, client: WebContentClientId, canvas_id: u64, now: MonotonicTime) {
        let _entered = self.enter();

        let context_ids: Vec<CompositorContextId> = self
            .contexts
            .iter()
            .filter(|(context_id, context)| {
                context.borrow().is_owned_by(client)
                    && self
                        .presenting_of(**context_id)
                        .is_some_and(|presenting| presenting.borrow().draws_canvas(canvas_id))
            })
            .map(|(&context_id, _)| context_id)
            .collect();
        for context_id in context_ids {
            self.present_changed_live_content(context_id, now);
        }
        self.finish_entry(now);
    }

    pub fn paint_screenshot(
        &self,
        context_id: CompositorContextId,
        target: &SurfaceHandle,
        now: MonotonicTime,
    ) -> bool {
        let _entered = self.enter();

        let painted = self.paint_screenshot_inner(context_id, target, now);
        self.finish_entry(now);
        painted
    }

    pub fn presented_frame_for_snapshot(
        &self,
        context_id: CompositorContextId,
        now: MonotonicTime,
    ) -> Option<(SurfaceHandle, IntSize)> {
        let _entered = self.enter();

        self.presented_frame_for_snapshot_inner(context_id, now)
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
        let installed = context.borrow_mut().install_display_list_update(
            display_list,
            visual_context_tree,
            scroll_state_snapshot,
            now,
        );
        self.finish_entry(now);
        installed
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
        let replaced = context.borrow_mut().update_visual_context_tree(
            visual_context_tree,
            scroll_state_snapshot,
            keyboard_scroll_state,
            now,
        );
        self.finish_entry(now);
        replaced
    }

    // Ends the fling of a context that is hidden, whether or not it was visible. Answers whether the context became
    // visible.
    pub fn set_visibility(
        &self,
        context_id: CompositorContextId,
        visibility: ContextVisibility,
        now: MonotonicTime,
    ) -> bool {
        let _entered = self.enter();

        let Some(context) = self.context_if_present(context_id) else {
            return false;
        };
        if visibility == ContextVisibility::Hidden {
            let event = context.borrow_mut().end_scroll_fling();
            self.dispatch_scroll_fling_step(context_id, context, event, now);
        }
        let became_visible = context.borrow().visibility() != visibility && visibility == ContextVisibility::Visible;
        context.borrow_mut().set_visibility(visibility);
        if became_visible {
            self.resume_presentation_after_becoming_visible(context_id, now);
        }
        self.finish_entry(now);
        became_visible
    }

    pub fn timer_fired(&self, context_id: CompositorContextId, timer: ContextTimer, now: MonotonicTime) {
        let _entered = self.enter();

        let Some(context) = self.context_if_present(context_id) else {
            return;
        };
        match timer {
            ContextTimer::BackingStoreShrink => {
                if let Some(presenting) = self.presenting_of(context_id) {
                    presenting.borrow_mut().finish_window_resize();
                }
                self.resize_backing_stores_if_needed(context_id, now);
            }
            ContextTimer::SurplusStoreRetirement => self.retire_idle_surplus_backing_stores(context_id),
            ContextTimer::LayerDrop => {
                if let Some(presenting) = self.presenting_of(context_id) {
                    let environment = self.environment(
                        context_id,
                        now,
                        &libcompositing_rust::display_list::replay::NoCompositedContexts,
                    );
                    presenting.borrow_mut().layer_drop_timer_fired(&environment);
                }
            }
            _ => {
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
        }
        self.finish_entry(now);
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

    pub(super) fn display_tick_inner(
        &self,
        context_id: CompositorContextId,
        frame_time: MonotonicTime,
        now: MonotonicTime,
        inputs: &DisplayTickInputs,
    ) -> DisplayTickResult {
        let context = self.context_if_present(context_id).expect("context exists");

        let has_active_animation_on_display =
            context.borrow_mut().needs_animation_frames(now) && inputs.context_is_on_tick_display;
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
            let overlay_fade_damage_rect = context.borrow_mut().advance_overlay_fades(frame_time);
            if let Some(overlay_fade_damage_rect) = overlay_fade_damage_rect {
                self.present_requests.borrow_mut().push(PresentRequest {
                    context_id,
                    viewport_rect: None,
                    forced_damage_rect: overlay_fade_damage_rect,
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
