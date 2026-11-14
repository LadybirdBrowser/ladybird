/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! When the frames of the contexts are presented, the frames in flight on the GPU, and whom each is delivered to.

use super::CompositorCore;
use crate::context_state::ContextState;
use crate::host::{PendingFrame, PresentScheduling, PresentationId};
use crate::presenting::{ContextPresenting, PresentingEnvironment};
use crate::raster_host::{
    FfiRemoteLayerFrame, FrameHandOff, StorePublication, StoreResizeOutcome, SublayersHandle, SurfaceHandle,
};
use libcompositing_rust::display_list::commands::CompositorContextId;
use libcompositing_rust::display_list::replay::{CompositedContexts, NoCompositedContexts, ResolvedCompositedContext};
use libcompositing_rust::ipc_types::{
    Compositing_ContextVisibility as ContextVisibility, compositor_presentation_client, compositor_web_content_client,
};
use libcompositing_rust::scrolling::wheel_gesture_identity::TouchpadGestureEnd;
use libcompositing_rust::time::MonotonicTime;
use libgfx_rust::{FloatMatrix4x4, FloatRect, IntRect, IntSize};
use std::cell::RefCell;

/// A frame submitted to the GPU, which is delivered once its work is done, or once it is scheduled where the window
/// server waits for it.
pub(crate) struct PendingAsyncPresent {
    token: u64,
    context_id: CompositorContextId,
    viewport_rect: IntRect,
    damage_rect: IntRect,
    bitmap_id: i32,
    device_pixel_ratio: f64,
    was_cancelled: bool,
    was_delivered: bool,
    // A frame that started rendering before its context was bound has no published buffer to present to.
    publish_to_presentation: bool,
    shows_in_remote_layer: bool,
    // A frame that changes only what the window server shows above the frame on screen.
    keeps_shown_store: bool,
    sublayers: Option<SublayersHandle>,
}

// What delivering a frame needs of its pending present.
struct Delivery {
    context_id: CompositorContextId,
    viewport_rect: IntRect,
    damage_rect: IntRect,
    bitmap_id: i32,
    device_pixel_ratio: f64,
    publish_to_presentation: bool,
    shows_in_remote_layer: bool,
    keeps_shown_store: bool,
    sublayers: Option<SublayersHandle>,
}

/// What a presentation makes of the frames and the scrolling of the contexts it shows.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PresentationCapabilities {
    pub synthesizes_scroll_momentum: bool,
    pub scrolls_elastically: bool,
    // Whether a press on a scrollbar track beside the thumb jumps there rather than paging toward it.
    pub scrollbar_track_press_jumps: bool,
    // LUID of the GPU adapter the presentation can present shared GPU textures on, if any.
    pub gpu_presentation_adapter_luid: Option<u64>,
    // Whether the presentation found it cannot present the shared GPU images it was given.
    pub cannot_present_gpu_images: bool,
}

/// The GPU the compositor renders with, as presenting shared stores depends on it.
#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct GpuBackend {
    pub has_gpu: bool,
    // Whether a presentation opens shared GPU stores only on the adapter they were made on.
    pub shared_stores_need_the_same_adapter: bool,
    pub adapter_luid: u64,
}

// The frames of the contexts nested in a context, which its replay draws, painted at once where they are behind.
struct ContextsNestedIn<'a> {
    core: &'a CompositorCore,
    parent_context_id: CompositorContextId,
    now: MonotonicTime,
}

impl CompositedContexts for ContextsNestedIn<'_> {
    fn resolve(
        &self,
        child_context_id: u64,
        destination_rect: FloatRect,
        canvas_matrix: &FloatMatrix4x4,
    ) -> Option<ResolvedCompositedContext> {
        self.core.resolve_composited_context(
            self.parent_context_id,
            CompositorContextId(child_context_id),
            destination_rect,
            canvas_matrix,
            self.now,
        )
    }
}

impl CompositorCore {
    pub(super) fn presenting_of(&self, context_id: CompositorContextId) -> Option<&RefCell<ContextPresenting>> {
        self.presenting.get(&context_id)
    }

    pub(super) fn environment<'a>(
        &'a self,
        context_id: CompositorContextId,
        now: MonotonicTime,
        composited_contexts: &'a dyn CompositedContexts,
    ) -> PresentingEnvironment<'a> {
        PresentingEnvironment {
            context_id,
            raster: &self.raster,
            host: &self.host,
            now,
            has_gpu: self.gpu.has_gpu,
            window_server_may_show_high_dynamic_range: self.window_server_may_show_high_dynamic_range.get(),
            composited_contexts,
        }
    }

    fn root_context_id_of(&self, context_id: CompositorContextId) -> CompositorContextId {
        let mut current = context_id;
        for _ in 0..=self.contexts.len() {
            let Some(parent) = self
                .context_if_present(current)
                .and_then(|context| context.borrow().parent_context_id())
            else {
                return current;
            };
            if self.context_if_present(parent).is_none() {
                return current;
            }
            current = parent;
        }
        current
    }

    pub(super) fn context_is_effectively_visible_inner(&self, context_id: CompositorContextId) -> bool {
        self.context_if_present(self.root_context_id_of(context_id))
            .is_some_and(|root| root.borrow().visibility() == ContextVisibility::Visible)
    }

    // The display a context is shown on and its refresh rate: its own, or those of the nearest context it is nested in
    // that knows its display.
    pub(super) fn display_of(&self, context_id: CompositorContextId) -> (Option<u64>, f64) {
        let own_refresh_rate = self
            .presenting_of(context_id)
            .map_or(60.0, |presenting| presenting.borrow().display_refresh_rate);
        let mut current = Some(context_id);
        for _ in 0..=self.contexts.len() {
            let Some(id) = current else { break };
            if let Some(presenting) = self.presenting_of(id) {
                let presenting = presenting.borrow();
                if presenting.display_id.is_some() {
                    return (presenting.display_id, presenting.display_refresh_rate);
                }
            }
            current = self
                .context_if_present(id)
                .and_then(|context| context.borrow().parent_context_id());
        }
        (None, own_refresh_rate)
    }

    fn request_vsync_for(&self, context_id: CompositorContextId) {
        let (display_id, refresh_rate) = self.display_of(context_id);
        self.raster.request_vsync(display_id, refresh_rate);
    }

    /// Schedules the frames and display ticks that the call that is ending asked for.
    pub(super) fn finish_entry(&self, now: MonotonicTime) {
        loop {
            let requests = std::mem::take(&mut *self.present_requests.borrow_mut());
            let display_tick_requests = self.host.take_display_tick_requests();
            if requests.is_empty() && display_tick_requests.is_empty() {
                break;
            }
            for context_id in display_tick_requests {
                if self.presenting_of(context_id).is_some() && self.context_is_effectively_visible_inner(context_id) {
                    self.request_vsync_for(context_id);
                }
            }
            for request in requests {
                let Some(presenting) = self.presenting_of(request.context_id) else {
                    continue;
                };
                let viewport_rect = request
                    .viewport_rect
                    .or_else(|| presenting.borrow().viewport_rect_of_current_frame());
                let Some(viewport_rect) = viewport_rect else {
                    continue;
                };
                let frame = PendingFrame {
                    viewport_rect,
                    forced_damage_rect: request.forced_damage_rect,
                };
                match request.scheduling {
                    PresentScheduling::OnNextDisplayTick => self.schedule_present_frame(request.context_id, frame, now),
                    PresentScheduling::WithDisplayTickInProgress => presenting.borrow_mut().queue_present_frame(frame),
                }
            }
        }
    }

    pub(super) fn schedule_present_frame(
        &self,
        context_id: CompositorContextId,
        frame: PendingFrame,
        now: MonotonicTime,
    ) {
        let Some(presenting) = self.presenting_of(context_id) else {
            return;
        };
        presenting.borrow_mut().queue_present_frame(frame);
        self.schedule_pending_present_frame(context_id, now);
    }

    fn schedule_present_frame_repainting_everything(
        &self,
        context_id: CompositorContextId,
        viewport_rect: IntRect,
        now: MonotonicTime,
    ) {
        self.schedule_present_frame(context_id, PendingFrame::repainting_everything(viewport_rect), now);
    }

    pub(super) fn present_current_frame(&self, context_id: CompositorContextId, now: MonotonicTime) {
        let Some(presenting) = self.presenting_of(context_id) else {
            return;
        };
        let frame_rect = presenting.borrow().frame_rect_to_repaint();
        if let Some(frame_rect) = frame_rect {
            self.schedule_present_frame_repainting_everything(context_id, frame_rect, now);
        }
    }

    pub(super) fn schedule_pending_present_frame(&self, context_id: CompositorContextId, now: MonotonicTime) {
        let Some(presenting) = self.presenting_of(context_id) else {
            return;
        };
        if !presenting.borrow().presents_to_client {
            // A nested context is presented through its containing context, but its own async animations still need a
            // vsync source. The containing frame may already be up to date (and therefore not schedule a new present),
            // so explicitly keep the effective display's scheduler ticking while a nested animation is active.
            if let Some(parent) = self
                .context_if_present(context_id)
                .and_then(|context| context.borrow().parent_context_id())
            {
                self.present_current_frame(parent, now);
            }
            self.schedule_animation_frames_if_needed(context_id, now);
            return;
        }
        if !self.context_is_effectively_visible_inner(context_id) {
            return;
        }
        let mut presenting = presenting.borrow_mut();
        presenting.mark_pending_present_frame_scheduled();
        // NB: A context presented to a client asks for the ticks of its own display, which is the one it knows.
        self.raster
            .request_vsync(presenting.display_id, presenting.display_refresh_rate);
    }

    fn schedule_animation_frames_if_needed(&self, context_id: CompositorContextId, now: MonotonicTime) {
        let Some(context) = self.context_if_present(context_id) else {
            return;
        };
        let needs_animation_frames = context.borrow_mut().needs_animation_frames(now);
        if needs_animation_frames && self.context_is_effectively_visible_inner(context_id) {
            self.request_vsync_for(context_id);
        }
    }

    pub(super) fn schedule_pending_present_frame_if_unblocked(
        &self,
        context_id: CompositorContextId,
        now: MonotonicTime,
    ) {
        let Some(presenting) = self.presenting_of(context_id) else {
            return;
        };
        if !presenting
            .borrow()
            .can_schedule_pending_present_frame_if_unblocked(&self.raster, context_id)
        {
            return;
        }
        if self.try_present_frame_during_resize(context_id, now) {
            return;
        }
        self.schedule_pending_present_frame(context_id, now);
    }

    pub(super) fn try_present_frame_during_resize(&self, context_id: CompositorContextId, now: MonotonicTime) -> bool {
        let Some(presenting) = self.presenting_of(context_id) else {
            return false;
        };
        {
            let presenting = presenting.borrow();
            if !presenting.window_resize_in_progress() || !presenting.presents_to_client {
                return false;
            }
        }
        if !self.context_is_effectively_visible_inner(context_id) {
            return false;
        }
        let pending_frame = presenting
            .borrow_mut()
            .take_pending_present_frame_if_unblocked(&self.raster, context_id);
        let Some(pending_frame) = pending_frame else {
            return false;
        };
        // Rasterize resize frames as soon as a backing store is available, so the previous size does not remain
        // visible for another display tick. Preparing the frame owns requeuing any blocked frame.
        self.present_frame(context_id, pending_frame, now);
        // The display tick does more than present frames, such as updating the video sinks that are painted.
        self.request_vsync_for(context_id);
        true
    }

    pub(super) fn resolve_composited_context(
        &self,
        parent_context_id: CompositorContextId,
        child_context_id: CompositorContextId,
        destination_rect: FloatRect,
        canvas_matrix: &FloatMatrix4x4,
        now: MonotonicTime,
    ) -> Option<ResolvedCompositedContext> {
        let child_state = self.context_if_present(child_context_id)?;
        if child_state.try_borrow().ok()?.parent_context_id() != Some(parent_context_id) {
            return None;
        }
        // A context nested in itself, through others, is already being painted.
        let mut child_presenting = self.presenting_of(child_context_id)?.try_borrow_mut().ok()?;
        let is_rendering = self.raster.store_state(child_context_id).is_rendering;
        if child_presenting.update_composited_raster_transform(is_rendering, destination_rect, canvas_matrix) {
            let environment = self.environment(child_context_id, now, &NoCompositedContexts);
            let outcome = child_presenting.resize_backing_stores_if_needed(
                &environment,
                StorePublication::Unpublished,
                false,
                None,
            );
            assert!(matches!(
                outcome,
                StoreResizeOutcome::Unchanged | StoreResizeOutcome::ReallocatedOffscreen
            ));
        }
        if child_presenting.needs_rasterization() {
            let (display_list, nested_display_lists, frame_inputs) = {
                let mut child_state = child_state.try_borrow_mut().ok()?;
                (
                    child_state.display_list(),
                    child_state.nested_display_lists(),
                    child_state.frame_inputs(now),
                )
            };
            let nested_contexts = ContextsNestedIn {
                core: self,
                parent_context_id: child_context_id,
                now,
            };
            let environment = self.environment(child_context_id, now, &nested_contexts);
            child_presenting.present_synchronously(
                &environment,
                display_list.as_ref(),
                &nested_display_lists,
                frame_inputs,
            );
        }
        child_presenting.composited_surface()
    }

    pub(super) fn present_frame(
        &self,
        context_id: CompositorContextId,
        pending_frame: PendingFrame,
        now: MonotonicTime,
    ) {
        let Some(context) = self.context_if_present(context_id) else {
            return;
        };
        let Some(presenting) = self.presenting_of(context_id) else {
            return;
        };
        let (display_list, nested_display_lists, frame_inputs, bound_presentation) = {
            let mut context = context.borrow_mut();
            (
                context.display_list(),
                context.nested_display_lists(),
                context.frame_inputs(now),
                context.bound_presentation(),
            )
        };
        let nested_contexts = ContextsNestedIn {
            core: self,
            parent_context_id: context_id,
            now,
        };
        let environment = self.environment(context_id, now, &nested_contexts);
        let mut presenting = presenting.borrow_mut();
        let Some(prepared_frame) = presenting.prepare_frame(
            &environment,
            pending_frame,
            display_list.as_ref(),
            &nested_display_lists,
            frame_inputs,
        ) else {
            return;
        };

        // A frame without a store still waits for what was drawn into the surfaces that the window server shows above
        // the store, such as a canvas that presented.
        let token = self
            .raster
            .submit_frame(context_id, prepared_frame.rendered_surface.as_ref());
        let shows_in_remote_layer = presenting.store_publication() == StorePublication::ShownInRemoteLayer;
        // A frame that the window server shows nothing above needs no list, which a remote layer makes when it shows
        // the frame.
        let sublayers = (!prepared_frame.sublayers.is_empty())
            .then(|| self.raster.build_sublayers(context_id, &prepared_frame.sublayers));
        self.async_presents.borrow_mut().push(PendingAsyncPresent {
            token,
            context_id,
            viewport_rect: pending_frame.viewport_rect,
            damage_rect: prepared_frame.damage_rect,
            bitmap_id: prepared_frame.bitmap_id,
            device_pixel_ratio: prepared_frame.device_pixel_ratio,
            was_cancelled: false,
            was_delivered: false,
            publish_to_presentation: !shows_in_remote_layer && bound_presentation.is_some(),
            shows_in_remote_layer,
            keeps_shown_store: prepared_frame.rendered_surface.is_none(),
            sublayers,
        });
        presenting.did_submit_prepared_frame(pending_frame.viewport_rect);
        // Handing the frame to the client before its GPU work finishes keeps it ahead of the window server's deadline for
        // the next refresh. A client that reads frames from CPU memory does not wait for that work, so it gets each frame
        // once it is done.
        let hands_off_early = presenting.presents_to_client
            && self.gpu.has_gpu
            && !presenting.presents_from_cpu_memory
            && self.raster.frame_hand_off(context_id) == FrameHandOff::OnceGpuWorkIsScheduled;
        drop(presenting);
        if hands_off_early {
            self.raster.hand_off_once_gpu_work_is_scheduled(token);
        }
    }

    fn take_delivery(&self, present: &mut PendingAsyncPresent) -> Option<Delivery> {
        if present.was_cancelled
            && present.keeps_shown_store
            && !present.was_delivered
            && let Some(presenting) = self.presenting_of(present.context_id)
        {
            presenting.borrow_mut().did_deliver_frame_without_store();
        }
        if present.was_cancelled || std::mem::replace(&mut present.was_delivered, true) {
            return None;
        }
        Some(Delivery {
            context_id: present.context_id,
            viewport_rect: present.viewport_rect,
            damage_rect: present.damage_rect,
            bitmap_id: present.bitmap_id,
            device_pixel_ratio: present.device_pixel_ratio,
            publish_to_presentation: present.publish_to_presentation,
            shows_in_remote_layer: present.shows_in_remote_layer,
            keeps_shown_store: present.keeps_shown_store,
            sublayers: present.sublayers.take(),
        })
    }

    pub(super) fn gpu_work_finished_inner(&self, token: u64, now: MonotonicTime) {
        let present = {
            let mut presents = self.async_presents.borrow_mut();
            let Some(index) = presents.iter().position(|present| present.token == token) else {
                return;
            };
            presents.remove(index)
        };
        let mut present = present;
        if let Some(delivery) = self.take_delivery(&mut present) {
            self.deliver_rendered_frame(delivery, now);
        }
    }

    pub(super) fn frame_ready_for_hand_off_inner(&self, token: u64, now: MonotonicTime) {
        let delivery = {
            let mut presents = self.async_presents.borrow_mut();
            let Some(present) = presents.iter_mut().find(|present| present.token == token) else {
                return;
            };
            self.take_delivery(present)
        };
        if let Some(delivery) = delivery {
            self.deliver_rendered_frame(delivery, now);
        }
    }

    fn deliver_rendered_frame(&self, mut delivery: Delivery, now: MonotonicTime) {
        let context_id = delivery.context_id;
        let (Some(context), Some(presenting)) = (self.context_if_present(context_id), self.presenting_of(context_id))
        else {
            return;
        };
        let environment = self.environment(context_id, now, &NoCompositedContexts);
        let viewport_size = IntSize {
            width: delivery.viewport_rect.width,
            height: delivery.viewport_rect.height,
        };
        let presents_to_client = {
            let mut presenting = presenting.borrow_mut();
            if delivery.keeps_shown_store {
                presenting.did_deliver_frame_without_store();
            } else {
                presenting.did_finish_gpu_present(&environment, delivery.bitmap_id, viewport_size);
            }
            presenting.presents_to_client
        };
        let (bound_presentation, page_id, parent_context_id) = {
            let context = context.borrow();
            (
                context.bound_presentation(),
                context.page_id(),
                context.parent_context_id(),
            )
        };
        let presentation = if delivery.publish_to_presentation {
            bound_presentation
        } else {
            None
        };
        let damage_rect = delivery.damage_rect;
        let log_presented_frame = || {
            if let Some(page_id) = page_id {
                self.raster.log_presented_frame(page_id, damage_rect);
            }
        };
        if delivery.shows_in_remote_layer {
            log_presented_frame();
            self.show_frame_in_remote_layer(&mut delivery, viewport_size, now);
        } else if presents_to_client && presentation.is_none() {
            // Nobody shows the frame, so it is released on another turn, as a presentation would release it.
            self.raster.release_store_later(context_id, delivery.bitmap_id);
        } else if presents_to_client && let Some(presentation) = presentation {
            log_presented_frame();
            self.host.post_to_presentation(
                presentation,
                compositor_presentation_client::messages::DidPresentFrame {
                    context_id,
                    content_rect: delivery.viewport_rect,
                    damage_rect: delivery.damage_rect,
                    bitmap_id: delivery.bitmap_id,
                    device_pixel_ratio: delivery.device_pixel_ratio,
                },
            );
        }
        self.resize_backing_stores_if_needed(context_id, now);
        if let Some(parent_context_id) = parent_context_id {
            self.present_current_frame(parent_context_id, now);
        }
        self.schedule_pending_present_frame_if_unblocked(context_id, now);
    }

    fn show_frame_in_remote_layer(&self, delivery: &mut Delivery, viewport_size: IntSize, now: MonotonicTime) {
        let context_id = delivery.context_id;
        let laid_out_by_page = self
            .presenting_of(context_id)
            .is_some_and(|presenting| presenting.borrow().size_laid_out_by_page == viewport_size);
        // The sublayers go with the frame, which keeps them.
        let sublayers = delivery
            .sublayers
            .take()
            .unwrap_or_else(|| self.raster.build_sublayers(context_id, &[]));
        let frame = FfiRemoteLayerFrame {
            uses_store: !delivery.keeps_shown_store,
            painted_size: viewport_size,
            device_pixel_ratio: delivery.device_pixel_ratio,
            laid_out_by_page,
        };
        let shown = self.raster.show_frame_in_remote_layer(context_id, &frame, sublayers);
        match shown {
            None => self.stop_showing_context_in_remote_layer(context_id, now),
            Some(presentation_id) => self.host.post_to_presentation(
                PresentationId(presentation_id),
                compositor_presentation_client::messages::DidPresentFrameInRemoteLayer {
                    context_id,
                    content_rect: delivery.viewport_rect,
                    damage_rect: delivery.damage_rect,
                    device_pixel_ratio: delivery.device_pixel_ratio,
                },
            ),
        }
    }

    // The frames were drawn into stores of the way the context was shown before, so they are released without being
    // shown.
    fn stop_delivering_in_flight_frames_to_their_viewer(&self, context_id: CompositorContextId) {
        for present in self.async_presents.borrow_mut().iter_mut() {
            if present.context_id == context_id {
                present.publish_to_presentation = false;
                present.shows_in_remote_layer = false;
            }
        }
    }

    pub(super) fn cancel_pending_async_presents_for_context(&self, context_id: CompositorContextId) {
        for present in self.async_presents.borrow_mut().iter_mut() {
            if present.context_id == context_id {
                present.was_cancelled = true;
            }
        }
    }

    pub(super) fn stop_showing_context_in_remote_layer(&self, context_id: CompositorContextId, now: MonotonicTime) {
        let Some(presenting) = self.presenting_of(context_id) else {
            return;
        };
        presenting.borrow_mut().shown_in_remote_layer = false;
        self.stop_delivering_in_flight_frames_to_their_viewer(context_id);
        let environment = self.environment(context_id, now, &NoCompositedContexts);
        presenting.borrow_mut().invalidate_backing_stores(&environment);
        self.resize_backing_stores_if_needed(context_id, now);
        self.schedule_pending_present_frame_if_unblocked(context_id, now);
    }

    pub(super) fn start_showing_context_in_remote_layer(&self, context_id: CompositorContextId, now: MonotonicTime) {
        let Some(presenting) = self.presenting_of(context_id) else {
            return;
        };
        presenting.borrow_mut().shown_in_remote_layer = true;
        self.stop_delivering_in_flight_frames_to_their_viewer(context_id);
        // The presentation no longer shows the stores it was given, so none of them is waiting to be released by it.
        self.raster.release_all_presented_stores(context_id);
        let environment = self.environment(context_id, now, &NoCompositedContexts);
        presenting.borrow_mut().invalidate_backing_stores(&environment);
        self.resize_backing_stores_if_needed(context_id, now);
        self.schedule_pending_present_frame_if_unblocked(context_id, now);
    }

    pub(super) fn gpu_sharing_allowed(&self, context_id: CompositorContextId) -> bool {
        if self
            .presenting_of(context_id)
            .is_some_and(|presenting| presenting.borrow().presents_from_cpu_memory)
        {
            return false;
        }
        let capabilities = self
            .context_if_present(context_id)
            .and_then(|context| context.borrow().bound_presentation())
            .and_then(|presentation| self.presentations.borrow().get(&presentation).copied());
        if self.gpu.shared_stores_need_the_same_adapter {
            // Shared GPU textures can only be opened by the presentation if it presents on the same adapter.
            return capabilities.is_some_and(|capabilities| {
                self.gpu.has_gpu && capabilities.gpu_presentation_adapter_luid == Some(self.gpu.adapter_luid)
            });
        }
        !capabilities.is_some_and(|capabilities| capabilities.cannot_present_gpu_images)
    }

    pub(super) fn resize_backing_stores_if_needed(&self, context_id: CompositorContextId, now: MonotonicTime) {
        let (Some(context), Some(presenting)) = (self.context_if_present(context_id), self.presenting_of(context_id))
        else {
            return;
        };
        let bound_presentation = context.borrow().bound_presentation();
        let publication = if presenting.borrow().shown_in_remote_layer {
            StorePublication::ShownInRemoteLayer
        } else if bound_presentation.is_some() {
            StorePublication::Published
        } else {
            StorePublication::Unpublished
        };
        let gpu_sharing_allowed = self.gpu_sharing_allowed(context_id);
        let environment = self.environment(context_id, now, &NoCompositedContexts);
        let outcome = presenting.borrow_mut().resize_backing_stores_if_needed(
            &environment,
            publication,
            gpu_sharing_allowed,
            bound_presentation.map(|presentation| presentation.0),
        );
        match outcome {
            StoreResizeOutcome::Unchanged | StoreResizeOutcome::ReallocatedOffscreen => {}
            StoreResizeOutcome::ReallocatedForViewer => self.present_current_frame(context_id, now),
            StoreResizeOutcome::Failed => {
                self.raster.stop_showing_in_remote_layer(context_id);
                self.stop_showing_context_in_remote_layer(context_id, now);
            }
        }
    }

    pub(super) fn add_backing_store_for_pending_frame_if_needed(&self, context_id: CompositorContextId) {
        let (Some(context), Some(presenting)) = (self.context_if_present(context_id), self.presenting_of(context_id))
        else {
            return;
        };
        let (bound_presentation, display_list) = {
            let context = context.borrow();
            (context.bound_presentation(), context.display_list())
        };
        let mut presenting = presenting.borrow_mut();
        if bound_presentation.is_none() && !presenting.shown_in_remote_layer {
            return;
        }
        if !presenting.wants_backing_store_for_pending_frame(display_list.as_ref(), &self.raster, context_id) {
            return;
        }
        let publishing_presentation = bound_presentation
            .filter(|_| presenting.store_publication() == StorePublication::Published)
            .map(|presentation| presentation.0);
        if self
            .raster
            .add_store_if_window_server_reads_every_released_store(context_id, publishing_presentation)
        {
            presenting.schedule_surplus_store_retirement(context_id, &self.host);
        }
    }

    pub(super) fn retire_idle_surplus_backing_stores(&self, context_id: CompositorContextId) {
        let (Some(context), Some(presenting)) = (self.context_if_present(context_id), self.presenting_of(context_id))
        else {
            return;
        };
        let bound_presentation = context.borrow().bound_presentation();
        let mut presenting = presenting.borrow_mut();
        presenting.surplus_store_retirement_fired();
        let publishing_presentation = bound_presentation
            .filter(|_| presenting.store_publication() == StorePublication::Published)
            .map(|presentation| presentation.0);
        if self
            .raster
            .retire_idle_surplus_stores(context_id, publishing_presentation)
        {
            presenting.schedule_surplus_store_retirement(context_id, &self.host);
        }
    }

    // A context scrolls elastically, takes presses on scrollbar tracks, and ends touchpad gestures as the presentation of
    // its root context does. A platform that reports the momentum of a flick reports when the fingers lift for it.
    pub(super) fn update_scroll_capabilities(&self) {
        for (&context_id, context) in &self.contexts {
            let root = self.root_context_id_of(context_id);
            let capabilities = self
                .context_if_present(root)
                .and_then(|root| root.borrow().bound_presentation())
                .and_then(|presentation| self.presentations.borrow().get(&presentation).copied());
            let mut scroll_capabilities = crate::context_state::ScrollCapabilities::default();
            if let Some(capabilities) = capabilities {
                scroll_capabilities.synthesizes_scroll_momentum = capabilities.synthesizes_scroll_momentum;
                scroll_capabilities.scrolls_elastically = capabilities.scrolls_elastically;
                scroll_capabilities.scrollbar_track_press_jumps = capabilities.scrollbar_track_press_jumps;
                scroll_capabilities.touchpad_gesture_end = if capabilities.synthesizes_scroll_momentum {
                    TouchpadGestureEnd::AfterIdleDelay
                } else {
                    TouchpadGestureEnd::WhenFingersLift
                };
            }
            context.borrow_mut().set_scroll_capabilities(&scroll_capabilities);
        }
    }

    pub(super) fn unbind_presentation_inner(&self, context_id: CompositorContextId, now: MonotonicTime) {
        let Some(context) = self.context_if_present(context_id) else {
            return;
        };
        if context.borrow().bound_presentation().is_none() {
            return;
        }
        context.borrow_mut().set_bound_presentation(None);
        for present in self.async_presents.borrow_mut().iter_mut() {
            if present.context_id == context_id {
                present.publish_to_presentation = false;
            }
        }
        // Nobody is left to release the buffers the presentation was showing.
        self.raster.release_all_presented_stores(context_id);
        self.update_scroll_capabilities();
        if let Some(presenting) = self.presenting_of(context_id) {
            let environment = self.environment(context_id, now, &NoCompositedContexts);
            presenting.borrow_mut().invalidate_backing_stores(&environment);
        }
        self.resize_backing_stores_if_needed(context_id, now);
        self.schedule_pending_present_frame_if_unblocked(context_id, now);
    }

    pub(super) fn resume_presentation_after_becoming_visible(
        &self,
        root_context_id: CompositorContextId,
        now: MonotonicTime,
    ) {
        let context_ids: Vec<CompositorContextId> = self.contexts.keys().copied().collect();
        for context_id in context_ids {
            if self.root_context_id_of(context_id) != root_context_id {
                continue;
            }
            self.schedule_animation_frames_if_needed(context_id, now);
            let wants_ticks = self.presenting_of(context_id).is_some_and(|presenting| {
                let presenting = presenting.borrow();
                presenting.rendering_opportunity_request.is_requested() || presenting.clock_tick_request.is_requested()
            });
            if wants_ticks {
                self.request_vsync_for(context_id);
            }
        }
        let frame_rect = self
            .presenting_of(root_context_id)
            .and_then(|presenting| presenting.borrow().frame_rect_to_repaint());
        if let Some(frame_rect) = frame_rect {
            self.schedule_present_frame_repainting_everything(root_context_id, frame_rect, now);
        }
    }

    pub(super) fn set_parent_context_inner(
        &self,
        context_id: CompositorContextId,
        mut parent_context_id: Option<CompositorContextId>,
        now: MonotonicTime,
    ) {
        let Some(context) = self.context_if_present(context_id) else {
            return;
        };
        if let Some(parent) = parent_context_id {
            assert!(!context.borrow().presents_to_client());
            assert_ne!(parent, context_id);
            // The parent's context belongs to the process hosting the parent, which can destroy it before this
            // context's process hears that the parent is gone. The context is left without a parent then, as destroying
            // the parent would have left it.
            if self.context_if_present(parent).is_none() {
                parent_context_id = None;
            }
        }
        let current_parent_context_id = context.borrow().parent_context_id();
        if current_parent_context_id == parent_context_id {
            return;
        }
        self.clear_parent_context(context_id, now);
        context.borrow_mut().set_parent_context(parent_context_id);
        self.update_scroll_capabilities();
        let Some(parent_context_id) = parent_context_id else {
            return;
        };
        let has_frame = self.presenting_of(context_id).is_some_and(|presenting| {
            let presenting = presenting.borrow();
            presenting.has_latest_rendered_surface() || presenting.needs_rasterization()
        });
        if has_frame {
            self.present_current_frame(parent_context_id, now);
        }
    }

    pub(super) fn clear_parent_context(&self, context_id: CompositorContextId, now: MonotonicTime) {
        let Some(context) = self.context_if_present(context_id) else {
            return;
        };
        let Some(parent_context_id) = context.borrow().parent_context_id() else {
            return;
        };
        context.borrow_mut().set_parent_context(None);
        self.present_current_frame(parent_context_id, now);
    }

    fn post_rendering_opportunity(
        &self,
        context: &RefCell<ContextState>,
        context_id: CompositorContextId,
        frame_time: MonotonicTime,
        frame_interval: f64,
    ) {
        let web_content_client_id = context.borrow().web_content_client_id();
        self.host.post_to_web_content(
            web_content_client_id,
            compositor_web_content_client::messages::RenderingOpportunity {
                context_id,
                frame_time_nanoseconds: frame_time.nanoseconds(),
                frame_interval_milliseconds: frame_interval,
            },
        );
    }

    pub(super) fn request_rendering_opportunity_inner(
        &self,
        context_id: CompositorContextId,
        maximum_frames_per_second: f64,
        now: MonotonicTime,
    ) {
        let (Some(context), Some(presenting)) = (self.context_if_present(context_id), self.presenting_of(context_id))
        else {
            return;
        };
        if !presenting
            .borrow_mut()
            .rendering_opportunity_request
            .request(maximum_frames_per_second)
        {
            return;
        }
        if !self.context_is_effectively_visible_inner(context_id) {
            return;
        }
        let (display_id, display_refresh_rate) = self.display_of(context_id);
        // INTEROP: Like Chromium's missed BeginFrame delivery, reuse a recent display tick when a context requests its
        //          next opportunity after the tick has already happened. This keeps heavy frames from waiting an extra
        //          tick.
        if let Some(frame_time) = self.raster.most_recent_tick_time(display_id, now, display_refresh_rate) {
            let delivery = {
                let mut presenting = presenting.borrow_mut();
                let request = &mut presenting.rendering_opportunity_request;
                request.is_due(frame_time, display_refresh_rate).then(|| {
                    let frame_interval = request.frame_interval(display_refresh_rate);
                    request.did_deliver(frame_time);
                    frame_interval
                })
            };
            if let Some(frame_interval) = delivery {
                self.post_rendering_opportunity(context, context_id, frame_time, frame_interval);
                return;
            }
        }
        self.raster.request_vsync(display_id, display_refresh_rate);
    }

    pub(super) fn hurry_rendering_opportunity_inner(&self, context_id: CompositorContextId, now: MonotonicTime) {
        let (Some(context), Some(presenting)) = (self.context_if_present(context_id), self.presenting_of(context_id))
        else {
            return;
        };
        if !presenting.borrow().rendering_opportunity_request.is_requested()
            || !self.context_is_effectively_visible_inner(context_id)
        {
            return;
        }
        let (_, display_refresh_rate) = self.display_of(context_id);
        let frame_interval = {
            let mut presenting = presenting.borrow_mut();
            let frame_interval = presenting
                .rendering_opportunity_request
                .frame_interval(display_refresh_rate);
            presenting.rendering_opportunity_request.did_deliver(now);
            frame_interval
        };
        self.post_rendering_opportunity(context, context_id, now, frame_interval);
    }

    pub(super) fn vsync_inner(&self, display_id: Option<u64>, frame_time: MonotonicTime, now: MonotonicTime) {
        for context_id in self.raster.update_video_sinks() {
            self.present_changed_live_content(context_id, now);
        }
        let context_ids: Vec<CompositorContextId> = self.contexts.keys().copied().collect();

        // Keep this display's vsync ticking while any sink painted on it may require updates.
        for &context_id in &context_ids {
            let (context_display_id, display_refresh_rate) = self.display_of(context_id);
            if context_display_id != display_id || !self.context_is_effectively_visible_inner(context_id) {
                continue;
            }
            if self.raster.context_video_sinks_require_updates(context_id) {
                self.raster.request_vsync(display_id, display_refresh_rate);
            }
        }

        for context_id in context_ids {
            let (Some(context), Some(presenting)) =
                (self.context_if_present(context_id), self.presenting_of(context_id))
            else {
                continue;
            };
            if !self.context_is_effectively_visible_inner(context_id) {
                presenting.borrow_mut().unschedule_pending_present_frame();
                continue;
            }
            let (context_display_id, display_refresh_rate) = self.display_of(context_id);
            let context_is_on_tick_display = context_display_id == display_id;

            let rendering_opportunity = {
                let mut presenting = presenting.borrow_mut();
                let request = &mut presenting.rendering_opportunity_request;
                if request.is_requested() && context_is_on_tick_display {
                    if request.is_due(frame_time, display_refresh_rate) {
                        let frame_interval = request.frame_interval(display_refresh_rate);
                        request.did_deliver(frame_time);
                        Some(frame_interval)
                    } else {
                        self.raster.request_vsync(display_id, display_refresh_rate);
                        None
                    }
                } else {
                    None
                }
            };
            if let Some(frame_interval) = rendering_opportunity {
                self.post_rendering_opportunity(context, context_id, frame_time, frame_interval);
            }

            let present_is_scheduled_on_tick_display = {
                let presenting = presenting.borrow();
                presenting.pending_present_frame_scheduled() && presenting.display_id == display_id
            };
            let clock_tick_interval_in_milliseconds = {
                let mut presenting = presenting.borrow_mut();
                let request = &mut presenting.clock_tick_request;
                if request.is_requested() && context_is_on_tick_display {
                    if request.is_due(frame_time, display_refresh_rate) {
                        let interval = request.frame_interval(display_refresh_rate);
                        request.did_deliver(frame_time);
                        Some(interval)
                    } else {
                        self.raster.request_vsync(display_id, display_refresh_rate);
                        None
                    }
                } else {
                    None
                }
            };
            let tick = self.display_tick_inner(
                context_id,
                frame_time,
                now,
                &crate::host::DisplayTickInputs {
                    context_is_on_tick_display,
                    present_is_scheduled_on_tick_display,
                    clock_tick_interval_in_milliseconds,
                },
            );
            self.finish_entry(now);
            if !tick.presents_on_display {
                continue;
            }

            self.add_backing_store_for_pending_frame_if_needed(context_id);
            let pending_frame = presenting
                .borrow_mut()
                .take_pending_present_frame_if_unblocked(&self.raster, context_id);
            let Some(pending_frame) = pending_frame else {
                let present_is_scheduled = {
                    let presenting = presenting.borrow();
                    presenting.pending_present_frame_scheduled() && presenting.display_id == display_id
                };
                if present_is_scheduled || (tick.needs_animation_frames && context_is_on_tick_display) {
                    self.raster.request_vsync(display_id, display_refresh_rate);
                }
                continue;
            };
            if tick.needs_animation_frames {
                self.schedule_present_frame(
                    context_id,
                    PendingFrame::repainting_changes(pending_frame.viewport_rect),
                    now,
                );
            }
            self.present_frame(context_id, pending_frame, now);
        }
    }

    pub(super) fn paint_screenshot_inner(
        &self,
        context_id: CompositorContextId,
        target: &SurfaceHandle,
        now: MonotonicTime,
    ) -> bool {
        let (Some(context), Some(presenting)) = (self.context_if_present(context_id), self.presenting_of(context_id))
        else {
            return false;
        };
        let Some(display_list) = context.borrow().display_list() else {
            return false;
        };
        let nested_display_lists = context.borrow().nested_display_lists();
        let screenshot_inputs = context.borrow_mut().frame_inputs_for_screenshot(now);
        let nested_contexts = ContextsNestedIn {
            core: self,
            parent_context_id: context_id,
            now,
        };
        let environment = self.environment(context_id, now, &nested_contexts);
        presenting.borrow_mut().paint_screenshot(
            &environment,
            target,
            &display_list,
            &nested_display_lists,
            &screenshot_inputs.frame_inputs,
        );
        if !screenshot_inputs.scrollbar_fade_damage_rect.is_empty() {
            let viewport_rect = presenting.borrow().viewport_rect_of_current_frame();
            if let Some(viewport_rect) = viewport_rect {
                self.schedule_present_frame(
                    context_id,
                    PendingFrame {
                        viewport_rect,
                        forced_damage_rect: screenshot_inputs.scrollbar_fade_damage_rect,
                    },
                    now,
                );
            }
        }
        true
    }

    pub(super) fn presented_frame_for_snapshot_inner(
        &self,
        context_id: CompositorContextId,
        now: MonotonicTime,
    ) -> Option<(SurfaceHandle, IntSize)> {
        let presenting = self.presenting_of(context_id)?;
        if !presenting.borrow().presents_to_client {
            return None;
        }
        let environment = self.environment(context_id, now, &NoCompositedContexts);
        presenting.borrow_mut().presented_frame_for_snapshot(&environment)
    }

    // A video, canvas or image that the context draws changed, so its next frame shows the change.
    pub(super) fn present_changed_live_content(&self, context_id: CompositorContextId, now: MonotonicTime) {
        if !self.context_is_effectively_visible_inner(context_id) {
            return;
        }
        let rect = self
            .presenting_of(context_id)
            .and_then(|presenting| presenting.borrow().self_present_rect());
        if let Some(rect) = rect {
            self.schedule_present_frame(context_id, PendingFrame::repainting_changes(rect), now);
        }
    }

    pub(super) fn maximum_display_refresh_rate(&self) -> f64 {
        self.presenting
            .values()
            .map(|presenting| presenting.borrow().display_refresh_rate)
            .fold(60.0, f64::max)
    }
}
