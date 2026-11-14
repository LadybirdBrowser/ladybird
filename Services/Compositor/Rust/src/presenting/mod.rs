/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! How the frames of one context are presented: which frame is next, what of it to repaint, the layers it is painted
//! in, and the rasters of it the backing stores and the window server hold. The painting itself is done by the raster
//! host.

mod damage;
mod layers;
pub mod pacing;

use crate::frame_inputs::FrameInputs;
use crate::host::{ContextTimer, Host, PendingFrame};
use crate::nested_display_lists::NestedDisplayListStore;
use crate::raster_host::{
    FfiStoreResize, FfiSublayer, PaintingResources, RasterHost, StorePublication, StoreResizeOutcome, SurfaceBlend,
    SurfaceHandle,
};
use damage::{PaintedFrame, StoreDamage};
use layers::ContextLayers;
use libcompositing_rust::display_list::commands::CompositorContextId;
use libcompositing_rust::display_list::replay::{
    CompositedContexts, ReplayFrame, ReplaySelection, ResolvedCompositedContext, replay_display_list,
};
use libcompositing_rust::host::RecordSpan;
use libcompositing_rust::scrolling::DisplayList;
use libcompositing_rust::time::{Duration, MonotonicTime};
use libgfx_rust::{Color, FloatMatrix4x4, FloatPoint, FloatRect, FloatSize, IntRect, IntSize, enclosing_int_rect};
use pacing::PacedTickRequest;
use std::rc::Rc;

const BACKING_STORE_SHRINK_DELAY: Duration = Duration::from_milliseconds(3000);
const SURPLUS_STORE_RETIREMENT_DELAY: Duration = Duration::from_milliseconds(3000);

/// What presenting a frame of a context works with besides the context's own state.
pub struct PresentingEnvironment<'a> {
    pub context_id: CompositorContextId,
    pub raster: &'a RasterHost,
    pub host: &'a Host,
    pub now: MonotonicTime,
    pub has_gpu: bool,
    pub window_server_may_show_high_dynamic_range: bool,
    pub composited_contexts: &'a dyn CompositedContexts,
}

/// A frame drawn into a store, or for a frame that changes only what the window server shows above the frame on
/// screen, into nothing.
pub struct PreparedFrame {
    pub rendered_surface: Option<SurfaceHandle>,
    pub bitmap_id: i32,
    pub damage_rect: IntRect,
    pub device_pixel_ratio: f64,
    // What the window server shows above the frame, bottom to top, lent by the presenting state until the next frame.
    pub sublayers: Vec<FfiSublayer>,
}

// The display list, the display lists it nests and the frame inputs a frame is painted with.
pub(crate) struct FrameToPaint<'a> {
    pub display_list: &'a Rc<DisplayList>,
    pub nested_display_lists: &'a Rc<NestedDisplayListStore>,
    pub frame_inputs: &'a FrameInputs,
}

// Bounds the embedding scale of a nested context's raster, so that the two backing stores together use at most 128 MiB,
// even when an embedding transform magnifies a large iframe.
fn bounded_composited_raster_scale(mut scale: FloatSize, viewport_size: IntSize) -> FloatSize {
    if viewport_size.width <= 0 || viewport_size.height <= 0 {
        return FloatSize {
            width: 1.0,
            height: 1.0,
        };
    }
    scale = FloatSize {
        width: scale.width.clamp(0.25, 5.0),
        height: scale.height.clamp(0.25, 5.0),
    };
    // Reserve a pixel on each axis for fractional placement.
    const MAXIMUM_DIMENSION: f32 = 16384.0;
    const MAXIMUM_PIXELS: f32 = 16.0 * 1024.0 * 1024.0;
    scale.width = scale.width.min((MAXIMUM_DIMENSION - 1.0) / viewport_size.width as f32);
    scale.height = scale
        .height
        .min((MAXIMUM_DIMENSION - 1.0) / viewport_size.height as f32);
    let pixel_count = (viewport_size.width as f32 * scale.width) * (viewport_size.height as f32 * scale.height);
    const MAXIMUM_CONTENT_PIXELS: f32 = MAXIMUM_PIXELS - 2.0 * MAXIMUM_DIMENSION - 1.0;
    if pixel_count > MAXIMUM_CONTENT_PIXELS {
        let factor = (MAXIMUM_CONTENT_PIXELS / pixel_count).sqrt();
        scale.width *= factor;
        scale.height *= factor;
    }
    scale
}

fn rect_of_size(size: IntSize) -> IntRect {
    IntRect::new(0, 0, size.width, size.height)
}

pub struct ContextPresenting {
    pub presents_to_client: bool,
    pub display_id: Option<u64>,
    pub display_refresh_rate: f64,
    // Whether the presentation shows this context's frames from CPU memory for now, as when the UI paints it itself.
    pub presents_from_cpu_memory: bool,
    pub shown_in_remote_layer: bool,
    // The viewport size of the frame that the page presented last, which it laid out at.
    pub size_laid_out_by_page: IntSize,
    pub rendering_opportunity_request: PacedTickRequest,
    pub clock_tick_request: PacedTickRequest,

    viewport_size: IntSize,
    device_pixel_ratio: f64,
    window_resize_in_progress: bool,

    pending_present_frame: Option<PendingFrame>,
    pending_present_frame_scheduled: bool,
    presented_frame: Option<IntRect>,

    store_publication: StorePublication,
    frame_without_store_in_flight: bool,
    latest_rendered_surface: Option<SurfaceHandle>,
    // The frame that the latest rendered surface holds, which the presented frame runs ahead of while its GPU work is in
    // flight.
    size_of_latest_rendered_frame: IntSize,
    store_damage: StoreDamage,
    damage_surface: Option<SurfaceHandle>,
    surplus_store_retirement_is_scheduled: bool,

    unbounded_raster_scale_of_embedding: FloatSize,
    raster_scale: FloatSize,
    raster_translation: FloatPoint,

    painted_frame: Option<PaintedFrame>,
    layers: ContextLayers,
}

impl ContextPresenting {
    pub fn new(presents_to_client: bool) -> Self {
        Self {
            presents_to_client,
            display_id: None,
            display_refresh_rate: 60.0,
            presents_from_cpu_memory: false,
            shown_in_remote_layer: false,
            size_laid_out_by_page: IntSize::default(),
            rendering_opportunity_request: PacedTickRequest::default(),
            clock_tick_request: PacedTickRequest::default(),
            viewport_size: IntSize::default(),
            device_pixel_ratio: 1.0,
            window_resize_in_progress: false,
            pending_present_frame: None,
            pending_present_frame_scheduled: false,
            presented_frame: None,
            store_publication: StorePublication::Unpublished,
            frame_without_store_in_flight: false,
            latest_rendered_surface: None,
            size_of_latest_rendered_frame: IntSize::default(),
            store_damage: StoreDamage::default(),
            damage_surface: None,
            surplus_store_retirement_is_scheduled: false,
            unbounded_raster_scale_of_embedding: FloatSize {
                width: 1.0,
                height: 1.0,
            },
            raster_scale: FloatSize {
                width: 1.0,
                height: 1.0,
            },
            raster_translation: FloatPoint::default(),
            painted_frame: None,
            layers: ContextLayers::default(),
        }
    }

    pub fn store_publication(&self) -> StorePublication {
        self.store_publication
    }

    // The window server reads what it shows above the store straight from GPU memory.
    pub fn remote_layer_may_show_media(&self, has_gpu: bool) -> bool {
        self.shown_in_remote_layer && has_gpu && !self.presents_from_cpu_memory
    }

    pub fn set_viewport_size(
        &mut self,
        viewport_size: IntSize,
        device_pixel_ratio: f64,
        window_resize_in_progress: bool,
    ) {
        self.viewport_size = viewport_size;
        self.device_pixel_ratio = device_pixel_ratio;
        self.window_resize_in_progress = window_resize_in_progress;
    }

    pub fn window_resize_in_progress(&self) -> bool {
        self.window_resize_in_progress
    }

    pub fn schedule_backing_store_shrink(&self, context_id: CompositorContextId, host: &Host) {
        host.set_timer(context_id, ContextTimer::BackingStoreShrink, BACKING_STORE_SHRINK_DELAY);
    }

    pub fn finish_window_resize(&mut self) {
        self.window_resize_in_progress = false;
    }

    // The viewport rect of the next frame, or of the frame presented last, at the viewport size.
    pub fn viewport_rect_of_current_frame(&self) -> Option<IntRect> {
        if self.viewport_size.is_empty() {
            return None;
        }
        let mut viewport_rect = match &self.pending_present_frame {
            Some(pending_frame) => pending_frame.viewport_rect,
            None => self.presented_frame.unwrap_or_default(),
        };
        viewport_rect.width = self.viewport_size.width;
        viewport_rect.height = self.viewport_size.height;
        Some(viewport_rect)
    }

    /// Resizes the stores to the raster size the context presents at, answering whether they were reallocated.
    pub fn resize_backing_stores_if_needed(
        &mut self,
        env: &PresentingEnvironment<'_>,
        mut publication: StorePublication,
        gpu_sharing_allowed: bool,
        presentation_id: Option<u64>,
    ) -> StoreResizeOutcome {
        if env.raster.store_state(env.context_id).is_rendering {
            return StoreResizeOutcome::Unchanged;
        }
        if !self.presents_to_client {
            // A resize can arrive before the parent's next replay supplies the new embedding transform. Bound the
            // existing embedding's scale for the new viewport before allocating backing stores. Bounding the already
            // bounded scale instead can round it differently, which would discard the raster on every replay.
            let scale = bounded_composited_raster_scale(self.unbounded_raster_scale_of_embedding, self.viewport_size);
            if self.raster_scale != scale {
                self.raster_scale = scale;
                self.latest_rendered_surface = None;
                self.painted_frame = None;
            }
            publication = StorePublication::Unpublished;
        }
        let outcome = env.raster.resize_stores(
            env.context_id,
            &FfiStoreResize {
                raster_size: self.raster_size(),
                window_resize_in_progress: self.window_resize_in_progress,
                publication,
                gpu_sharing_allowed,
                has_presentation: presentation_id.is_some(),
                presentation_id: presentation_id.unwrap_or_default(),
            },
        );
        if outcome != StoreResizeOutcome::Unchanged {
            self.store_publication = publication;
            self.latest_rendered_surface = None;
            self.painted_frame = None;
            self.store_damage.forget_every_store();
        }
        outcome
    }

    pub fn invalidate_backing_stores(&mut self, env: &PresentingEnvironment<'_>) {
        env.raster.invalidate_stores(env.context_id);
        self.frame_without_store_in_flight = false;
    }

    pub fn wants_backing_store_for_pending_frame(
        &self,
        display_list: Option<&Rc<DisplayList>>,
        raster: &RasterHost,
        context_id: CompositorContextId,
    ) -> bool {
        self.presents_to_client
            && self.pending_present_frame.is_some()
            && self.can_render_frame(display_list, raster, context_id)
    }

    /// Schedules the retirement of the stores added for a burst of frames, unless it is scheduled already.
    pub fn schedule_surplus_store_retirement(&mut self, context_id: CompositorContextId, host: &Host) {
        if self.surplus_store_retirement_is_scheduled {
            return;
        }
        self.surplus_store_retirement_is_scheduled = true;
        host.set_timer(
            context_id,
            ContextTimer::SurplusStoreRetirement,
            SURPLUS_STORE_RETIREMENT_DELAY,
        );
    }

    pub fn surplus_store_retirement_fired(&mut self) {
        self.surplus_store_retirement_is_scheduled = false;
    }

    pub fn did_finish_gpu_present(&mut self, env: &PresentingEnvironment<'_>, bitmap_id: i32, frame_size: IntSize) {
        env.raster.complete_rendering(env.context_id, bitmap_id);
        self.latest_rendered_surface = env.raster.latest_rendered_store_surface(env.context_id);
        self.size_of_latest_rendered_frame = frame_size;
    }

    pub fn did_deliver_frame_without_store(&mut self) {
        self.frame_without_store_in_flight = false;
    }

    pub fn has_latest_rendered_surface(&self) -> bool {
        self.latest_rendered_surface.is_some()
    }

    /// Answers whether the raster has to be made again, at the scale and phase the embedding now draws it at.
    pub fn update_composited_raster_transform(
        &mut self,
        is_rendering: bool,
        destination_rect: FloatRect,
        transform: &FloatMatrix4x4,
    ) -> bool {
        if self.presents_to_client || is_rendering || self.viewport_size.is_empty() || destination_rect.is_empty() {
            return false;
        }
        let m = &transform.elements;
        // Display list coordinates already include device scale and page zoom. Only the embedding transform and
        // the viewport-to-destination mapping belong here; neither changes the child's layout or scroll geometry.
        let has_perspective = m[3][0] != 0.0 || m[3][1] != 0.0 || m[3][3] != 1.0;
        let mut scale_x = 1.0f32;
        let mut scale_y = 1.0f32;
        if !has_perspective {
            scale_x = ak::math::hypot(m[0][0], m[1][0]) * destination_rect.width / self.viewport_size.width as f32;
            scale_y = ak::math::hypot(m[0][1], m[1][1]) * destination_rect.height / self.viewport_size.height as f32;
        }
        if !scale_x.is_finite() || !scale_y.is_finite() {
            return false;
        }
        self.unbounded_raster_scale_of_embedding = FloatSize {
            width: scale_x,
            height: scale_y,
        };

        // Perspective keeps the native resolution until we can choose a scale from the visible projected region.
        let scale = bounded_composited_raster_scale(self.unbounded_raster_scale_of_embedding, self.viewport_size);

        let mut translation = FloatPoint::default();
        if !has_perspective && m[0][1] == 0.0 && m[1][0] == 0.0 && scale.width == scale_x && scale.height == scale_y {
            let mut x = m[0][0] * destination_rect.x + m[0][3];
            let mut y = m[1][1] * destination_rect.y + m[1][3];
            if !x.is_finite() || !y.is_finite() {
                return false;
            }
            // Mirrored placements need the opposite fractional phase so the remaining image transform still
            // lands on integer device pixels.
            if m[0][0] < 0.0 {
                x = -x;
            }
            if m[1][1] < 0.0 {
                y = -y;
            }
            translation = FloatPoint {
                x: x - x.floor(),
                y: y - y.floor(),
            };
        }

        if self.raster_scale == scale && self.raster_translation == translation {
            return false;
        }
        self.raster_scale = scale;
        self.raster_translation = translation;
        self.latest_rendered_surface = None;
        self.painted_frame = None;
        true
    }

    pub fn composited_surface(&self) -> Option<ResolvedCompositedContext> {
        let surface = self.latest_rendered_surface.as_ref()?;
        let painted_frame = self.painted_frame.as_ref()?;
        Some(ResolvedCompositedContext {
            surface: surface.as_ptr(),
            content_rect: FloatRect::new(
                self.raster_translation.x,
                self.raster_translation.y,
                painted_frame.viewport_size.width as f32 * self.raster_scale.width,
                painted_frame.viewport_size.height as f32 * self.raster_scale.height,
            ),
        })
    }

    pub fn queue_present_frame(&mut self, pending_frame: PendingFrame) {
        let Some(queued) = &mut self.pending_present_frame else {
            self.pending_present_frame = Some(pending_frame);
            return;
        };
        if queued.viewport_rect.width != pending_frame.viewport_rect.width
            || queued.viewport_rect.height != pending_frame.viewport_rect.height
        {
            self.pending_present_frame = Some(PendingFrame::repainting_everything(pending_frame.viewport_rect));
            return;
        }
        queued.viewport_rect = pending_frame.viewport_rect;
        queued.forced_damage_rect = queued.forced_damage_rect.united(pending_frame.forced_damage_rect);
    }

    pub fn mark_pending_present_frame_scheduled(&mut self) {
        self.pending_present_frame_scheduled = true;
    }

    pub fn unschedule_pending_present_frame(&mut self) {
        self.pending_present_frame_scheduled = false;
    }

    pub fn pending_present_frame_scheduled(&self) -> bool {
        self.pending_present_frame_scheduled
    }

    pub fn can_schedule_pending_present_frame_if_unblocked(
        &self,
        raster: &RasterHost,
        context_id: CompositorContextId,
    ) -> bool {
        !self.is_present_blocked(raster, context_id)
            && self.pending_present_frame.is_some()
            && !self.pending_present_frame_scheduled
    }

    pub fn take_pending_present_frame_if_unblocked(
        &mut self,
        raster: &RasterHost,
        context_id: CompositorContextId,
    ) -> Option<PendingFrame> {
        if self.is_present_blocked(raster, context_id) {
            return None;
        }
        self.pending_present_frame_scheduled = false;
        self.pending_present_frame.take()
    }

    pub fn needs_rasterization(&self) -> bool {
        self.pending_present_frame.is_some()
            || (self.latest_rendered_surface.is_none() && self.presented_frame.is_some())
    }

    pub fn frame_rect_to_repaint(&self) -> Option<IntRect> {
        match &self.pending_present_frame {
            Some(pending_frame) => Some(pending_frame.viewport_rect),
            None => self.presented_frame,
        }
    }

    pub fn self_present_rect(&self) -> Option<IntRect> {
        if self.presented_frame.is_some() {
            return self.presented_frame;
        }
        (!self.viewport_size.is_empty()).then(|| rect_of_size(self.viewport_size))
    }

    pub fn draws_canvas(&self, canvas_id: u64) -> bool {
        self.painted_frame
            .as_ref()
            .is_some_and(|painted_frame| painted_frame.draws_canvas(canvas_id))
    }

    pub fn did_submit_prepared_frame(&mut self, viewport_rect: IntRect) {
        self.presented_frame = Some(viewport_rect);
    }

    fn is_present_blocked(&self, raster: &RasterHost, context_id: CompositorContextId) -> bool {
        let state = raster.store_state(context_id);
        state.is_rendering || self.frame_without_store_in_flight || !state.has_available_store
    }

    fn can_render_frame(
        &self,
        display_list: Option<&Rc<DisplayList>>,
        raster: &RasterHost,
        context_id: CompositorContextId,
    ) -> bool {
        display_list.is_some() && raster.store_state(context_id).is_valid
    }

    fn raster_size(&self) -> IntSize {
        if self.viewport_size.is_empty() {
            return IntSize::default();
        }
        if self.presents_to_client {
            return self.viewport_size;
        }
        IntSize {
            width: (self.viewport_size.width as f32 * self.raster_scale.width + self.raster_translation.x).ceil()
                as i32,
            height: (self.viewport_size.height as f32 * self.raster_scale.height + self.raster_translation.y).ceil()
                as i32,
        }
    }

    fn raster_damage_rect(&self, damage_rect: IntRect) -> IntRect {
        if damage_rect.is_empty() {
            return IntRect::default();
        }
        let raster_rect = rect_of_size(self.raster_size());
        if damage_rect.contains_rect(rect_of_size(self.viewport_size)) {
            return raster_rect;
        }
        let rect = damage_rect.to_float();
        let scaled = FloatRect::new(
            rect.x * self.raster_scale.width + self.raster_translation.x,
            rect.y * self.raster_scale.height + self.raster_translation.y,
            rect.width * self.raster_scale.width,
            rect.height * self.raster_scale.height,
        );
        enclosing_int_rect(scaled).intersected(raster_rect)
    }

    /// The frame painted into a store, or none where it is blocked, held back, or has nothing new to show.
    pub fn prepare_frame(
        &mut self,
        env: &PresentingEnvironment<'_>,
        pending_frame: PendingFrame,
        display_list: Option<&Rc<DisplayList>>,
        nested_display_lists: &Rc<NestedDisplayListStore>,
        frame_inputs: Option<FrameInputs>,
    ) -> Option<PreparedFrame> {
        if self.is_present_blocked(env.raster, env.context_id) {
            self.queue_present_frame(pending_frame);
            return None;
        }
        if !self.can_render_frame(display_list, env.raster, env.context_id) {
            self.presented_frame = Some(pending_frame.viewport_rect);
            return None;
        }
        let display_list = display_list.expect("a frame that can render has a display list");
        let frame_inputs = frame_inputs.expect("a context with a display list has frame inputs");
        let live_content = self.live_content(env, display_list, nested_display_lists);
        let frame = FrameToPaint {
            display_list,
            nested_display_lists,
            frame_inputs: &frame_inputs,
        };
        let mut frame_damage = self.frame_damage_for(env, &pending_frame, &frame, &live_content);
        let mut damage_rect = frame_damage.rect;
        // NB: A changed content rect must still reach the client even if the pixels are identical.
        if damage_rect.is_empty() && self.presented_frame == Some(pending_frame.viewport_rect) {
            let only_media_shown_by_window_server_changed =
                frame_damage.layers.shown_by_window_server && frame_damage.live_media_changed;
            self.commit_layer_update(env, frame_damage.layers);
            self.remember_painted_frame(
                pending_frame.viewport_rect,
                display_list,
                nested_display_lists,
                frame_inputs,
                &live_content,
            );
            if !only_media_shown_by_window_server_changed {
                return None;
            }
            self.frame_without_store_in_flight = true;
            return Some(PreparedFrame {
                rendered_surface: None,
                bitmap_id: -1,
                damage_rect: IntRect::default(),
                device_pixel_ratio: self.device_pixel_ratio,
                sublayers: self.sublayers_shown_by_window_server(&live_content),
            });
        }
        let raster_damage_rect = self.raster_damage_rect(damage_rect);
        let Some((bitmap_id, store)) = env.raster.acquire_store(env.context_id) else {
            self.queue_present_frame(pending_frame);
            return None;
        };
        let store_rect = rect_of_size(env.raster.surface_size(&store));
        let repaint_rect = self.store_damage.take(bitmap_id, raster_damage_rect, store_rect);
        let painted_rect = self.paint_frame(env, &store, &frame, repaint_rect, &mut frame_damage.layers);
        if painted_rect != repaint_rect {
            damage_rect = rect_of_size(IntSize {
                width: pending_frame.viewport_rect.width,
                height: pending_frame.viewport_rect.height,
            });
        }
        self.commit_layer_update(env, frame_damage.layers);
        self.remember_painted_frame(
            pending_frame.viewport_rect,
            display_list,
            nested_display_lists,
            frame_inputs,
            &live_content,
        );
        Some(PreparedFrame {
            rendered_surface: Some(store),
            bitmap_id,
            damage_rect,
            device_pixel_ratio: self.device_pixel_ratio,
            sublayers: self.sublayers_shown_by_window_server(&live_content),
        })
    }

    /// Paints the pending frame, or the frame presented last, into a store at once, for a context that its parent draws.
    pub fn present_synchronously(
        &mut self,
        env: &PresentingEnvironment<'_>,
        display_list: Option<&Rc<DisplayList>>,
        nested_display_lists: &Rc<NestedDisplayListStore>,
        frame_inputs: Option<FrameInputs>,
    ) -> bool {
        if !self.can_render_frame(display_list, env.raster, env.context_id) {
            return false;
        }
        let display_list = display_list.expect("a frame that can render has a display list");
        let frame_inputs = frame_inputs.expect("a context with a display list has frame inputs");
        // Don't race an async present already in flight for this context; its own completion will update the output.
        if self.is_present_blocked(env.raster, env.context_id) {
            return false;
        }
        let pending_frame = match self.pending_present_frame {
            Some(pending_frame) => pending_frame,
            None => match self.presented_frame {
                Some(presented_frame) => PendingFrame::repainting_everything(presented_frame),
                None => return false,
            },
        };
        let live_content = self.live_content(env, display_list, nested_display_lists);
        let frame = FrameToPaint {
            display_list,
            nested_display_lists,
            frame_inputs: &frame_inputs,
        };
        let mut frame_damage = self.frame_damage_for(env, &pending_frame, &frame, &live_content);
        let raster_damage_rect = self.raster_damage_rect(frame_damage.rect);
        let Some((bitmap_id, store)) = env.raster.acquire_store(env.context_id) else {
            return false;
        };
        let store_rect = rect_of_size(env.raster.surface_size(&store));
        let repaint_rect = self.store_damage.take(bitmap_id, raster_damage_rect, store_rect);
        let _ = self.paint_frame(env, &store, &frame, repaint_rect, &mut frame_damage.layers);
        self.commit_layer_update(env, frame_damage.layers);
        self.remember_painted_frame(
            pending_frame.viewport_rect,
            display_list,
            nested_display_lists,
            frame_inputs,
            &live_content,
        );
        env.raster.flush(&store);
        env.raster.complete_rendering(env.context_id, bitmap_id);
        self.latest_rendered_surface = env.raster.latest_rendered_store_surface(env.context_id);
        self.size_of_latest_rendered_frame = IntSize {
            width: pending_frame.viewport_rect.width,
            height: pending_frame.viewport_rect.height,
        };
        self.presented_frame = Some(pending_frame.viewport_rect);
        self.pending_present_frame = None;
        self.pending_present_frame_scheduled = false;
        true
    }

    /// Paints a screenshot into the target, without the overlays of the browser and at the raster's own scale.
    pub fn paint_screenshot(
        &mut self,
        env: &PresentingEnvironment<'_>,
        target: &SurfaceHandle,
        display_list: &Rc<DisplayList>,
        nested_display_lists: &Rc<NestedDisplayListStore>,
        frame_inputs: &FrameInputs,
    ) {
        let frame = FrameToPaint {
            display_list,
            nested_display_lists,
            frame_inputs,
        };
        self.paint_current_display_list(env, target, &frame, None, false, false);
        env.raster.flush(target);
    }

    /// The surface that holds the frame on screen as a whole, with what the window server shows above the store
    /// composited into it, and the size of the frame in it.
    pub fn presented_frame_for_snapshot(
        &mut self,
        env: &PresentingEnvironment<'_>,
    ) -> Option<(SurfaceHandle, IntSize)> {
        let latest_rendered_surface = self.latest_rendered_surface.clone()?;
        let mut source = latest_rendered_surface;
        // The store then holds only what is under the canvases, videos and overlays that the window server shows above it.
        if self.layers.window_server_shows_media()
            && self.layers.has_plan()
            && let Some(painted_frame) = self.painted_frame.take()
        {
            let source_size = env.raster.surface_size(&source);
            let composited = env.raster.create_surface(source_size, Some(&source));
            if let Some(composited) = composited {
                let source_rect = rect_of_size(source_size);
                env.raster
                    .draw_surface(&composited, &source, source_rect, source_rect, SurfaceBlend::Source);
                let frame = FrameToPaint {
                    display_list: &painted_frame.display_list,
                    nested_display_lists: &painted_frame.nested_display_lists,
                    frame_inputs: &painted_frame.frame_inputs,
                };
                let snapshot_env = PresentingEnvironment {
                    composited_contexts: &libcompositing_rust::display_list::replay::NoCompositedContexts,
                    ..*env
                };
                self.composite_layers_above_base(&snapshot_env, &composited, &frame, source_rect);
                paint_overlays(&snapshot_env, &composited, &painted_frame.frame_inputs, true);
                source = composited;
            }
            self.painted_frame = Some(painted_frame);
        }
        let source_size = env.raster.surface_size(&source);
        let frame_size = IntSize {
            width: self.size_of_latest_rendered_frame.width.min(source_size.width),
            height: self.size_of_latest_rendered_frame.height.min(source_size.height),
        };
        if frame_size.is_empty() {
            return None;
        }
        Some((source, frame_size))
    }

    pub fn layer_drop_timer_fired(&mut self, env: &PresentingEnvironment<'_>) {
        self.layers.drop_timer_fired(env);
    }

    // Returns the rect it painted, which grows to the whole frame when the frame has to hold media that the window
    // server was to show.
    fn paint_frame(
        &mut self,
        env: &PresentingEnvironment<'_>,
        surface: &SurfaceHandle,
        frame: &FrameToPaint<'_>,
        mut damage_rect: IntRect,
        layer_update: &mut layers::LayerUpdate,
    ) -> IntRect {
        if layer_update.has_plan() {
            if self.paint_layered_frame(env, surface, frame, damage_rect, layer_update) {
                return damage_rect;
            }
            // The sublayers that the window server was to show go away with this frame, so the store has to hold all
            // that they showed.
            if layer_update.shown_by_window_server {
                damage_rect = rect_of_size(self.raster_size());
            }
            layer_update.new_plan = None;
            layer_update.keeps_plan = false;
            layer_update.shown_by_window_server = false;
            self.layers.drop_layers(env);
            self.layers.repaint_everything_after_dropping_layers = true;
        }
        // NB: Only a frame shown on screen ages the rasters it did not use, so a screenshot keeps them.
        env.raster.start_raster_cache_frame(env.context_id);
        self.paint_current_display_list(env, surface, frame, Some(damage_rect), true, true);
        damage_rect
    }

    fn paint_current_display_list(
        &mut self,
        env: &PresentingEnvironment<'_>,
        surface: &SurfaceHandle,
        frame: &FrameToPaint<'_>,
        damage_rect: Option<IntRect>,
        paints_ui_overlays: bool,
        applies_raster_transform: bool,
    ) {
        let raster = env.raster;
        let surface_clear_color = frame.display_list.surface_clear_color().unwrap_or(Color::TRANSPARENT);
        let paint_display_list = |target: &SurfaceHandle| {
            replay_into(env, target, frame, ReplaySelection::default());
            paint_overlays(env, target, frame.frame_inputs, paints_ui_overlays);
        };

        if let Some(damage_rect) = damage_rect
            && !damage_rect.is_empty()
            && self.presents_to_client
            && (IntSize {
                width: damage_rect.width,
                height: damage_rect.height,
            }) != raster.surface_size(surface)
            && raster.surface_uses_gpu(surface)
        {
            let damage_size = IntSize {
                width: damage_rect.width,
                height: damage_rect.height,
            };
            if self
                .damage_surface
                .as_ref()
                .is_none_or(|damage_surface| raster.surface_size(damage_surface) != damage_size)
            {
                self.damage_surface = raster.create_surface(damage_size, Some(surface));
            }
            if let Some(damage_surface) = &self.damage_surface {
                raster.clear(damage_surface, surface_clear_color);
                let save_count = raster.save(damage_surface);
                raster.translate(damage_surface, -damage_rect.x as f32, -damage_rect.y as f32);
                paint_display_list(damage_surface);
                raster.restore_to_count(damage_surface, save_count);
                raster.draw_surface(
                    surface,
                    damage_surface,
                    rect_of_size(damage_size),
                    damage_rect,
                    SurfaceBlend::Source,
                );
                return;
            }
        }

        let save_count = raster.save(surface);
        if let Some(damage_rect) = damage_rect {
            raster.clip_rect(surface, damage_rect);
            raster.clear(surface, surface_clear_color);
        }
        if applies_raster_transform {
            raster.translate(surface, self.raster_translation.x, self.raster_translation.y);
            raster.scale(surface, self.raster_scale.width, self.raster_scale.height);
        }
        paint_display_list(surface);
        raster.restore_to_count(surface, save_count);
    }
}

// Replays the frame's display list into the target, through the painter of the context's page.
pub(crate) fn replay_into(
    env: &PresentingEnvironment<'_>,
    target: &SurfaceHandle,
    frame: &FrameToPaint<'_>,
    selection: ReplaySelection<'_>,
) {
    let frame_inputs = frame.frame_inputs;
    let display_list = frame.display_list;
    assert_eq!(
        display_list.compatible_visual_context_tree_structural_epoch(),
        frame_inputs.visual_context_tree.structural_epoch()
    );
    env.raster
        .paint(env.context_id, target, PaintingResources::Page, |painter| {
            let scrollbars = frame_inputs.scrollbars_shown_for_the_page();
            let replay_frame = ReplayFrame {
                scroll_offsets: frame_inputs.presented_scroll_state_snapshot.device_offsets(),
                scrollbars: &scrollbars,
                time_ns: frame_inputs.painted_at.nanoseconds(),
                nested_display_lists: frame.nested_display_lists.as_ref(),
                composited_contexts: env.composited_contexts,
            };
            let storage = display_list.storage();
            let tree = frame_inputs.visual_context_tree.as_arc();
            replay_display_list(
                tree,
                storage.bytes(),
                storage.command_runs(),
                storage.effect_clip_plan(tree),
                display_list.root_group_color(),
                &replay_frame,
                selection,
                painter,
            );
        });
}

pub(crate) fn replaying_only(record_spans: &[RecordSpan], skip_root_isolation_layer: bool) -> ReplaySelection<'_> {
    ReplaySelection {
        record_spans: Some(record_spans),
        skip_root_isolation_layer,
    }
}

// Paints what the compositor draws over the page: its scrollbars, and unless they are left out, the overlays of the
// browser.
pub(crate) fn paint_overlays(
    env: &PresentingEnvironment<'_>,
    target: &SurfaceHandle,
    frame_inputs: &FrameInputs,
    paints_ui_overlays: bool,
) {
    env.raster
        .paint(env.context_id, target, PaintingResources::Overlays, |painter| {
            frame_inputs.overlays.paint(painter, paints_ui_overlays);
        });
}
