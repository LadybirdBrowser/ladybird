/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! How the frames of one context are presented: which frame is next, what of it to repaint, the layers it is painted
//! in, and the rasters of it the backing stores and the window server hold. The painting itself is done by the raster
//! host.

mod damage;
mod layer_overlay;
mod layers;
pub mod pacing;
mod tiles;
mod window_server;

use crate::backing_stores::{AllocationOutcome, BackingStores, Publication, StorePublication, StoreResizeOutcome};
use crate::frame_inputs::FrameInputs;
use crate::host::{ContextTimer, Host, PendingFrame};
use crate::raster_host::{FfiSublayer, PaintingResources, RasterHost, SurfaceBlend};
use crate::remote_layers::{ShowingLayer, WindowServerObject};
use crate::surfaces::painting_surface::PaintingSurface;
use crate::surfaces::presentable_surface::PresentableSurface;
use damage::{PaintedFrame, StoreDamage};
use layer_overlay::LayerOverlaySwitch;
use layers::ContextLayers;
use libcompositing_rust::display_list::commands::CompositorContextId;
use libcompositing_rust::display_list::replay::{
    CompositedContexts, CompositingInsideEffects, NoCompositedContexts, ReplayFrame, ReplaySelection,
    ResolvedCompositedContext, composite_inside_effects, replay_display_list,
};
use libcompositing_rust::host::FfiRasterPainter;
use libcompositing_rust::ipc_types::Compositing_CompositorLayersDescription as CompositorLayersDescription;
use libcompositing_rust::scrolling::DisplayList;
use libcompositing_rust::time::{Duration, MonotonicTime};
use libgfx_rust::{Color, FloatMatrix4x4, FloatPoint, FloatRect, FloatSize, IntRect, IntSize, enclosing_int_rect};
use pacing::PacedTickRequest;
use std::rc::Rc;

const BACKING_STORE_SHRINK_DELAY: Duration = Duration::from_milliseconds(3000);
const SURPLUS_STORE_RETIREMENT_DELAY: Duration = Duration::from_milliseconds(3000);
// A tab the user left is likely to stay in the background for long, so once it was hidden for this long, its context
// keeps only the store that holds its latest frame. The wait keeps a user who switches back and forth between tabs from
// paying for new stores each time.
const TIME_HIDDEN_BEFORE_RETIRING_STORES: Duration = Duration::from_milliseconds(10_000);
// A store that is still rendered into, shown or read when the hidden context retires its stores is retired this much
// later.
const HIDDEN_STORE_RETIREMENT_RETRY_DELAY: Duration = Duration::from_milliseconds(3000);

pub trait NestedContexts: CompositedContexts {
    fn frame_generation_of(&self, child_context_id: CompositorContextId) -> Option<u64>;
}

impl NestedContexts for NoCompositedContexts {
    fn frame_generation_of(&self, _child_context_id: CompositorContextId) -> Option<u64> {
        None
    }
}

/// What presenting a frame of a context works with besides the context's own state.
pub struct PresentingEnvironment<'a> {
    pub context_id: CompositorContextId,
    /// The remote layer that shows the context, if one does.
    pub remote_layer: Option<ShowingLayer>,
    pub raster: &'a RasterHost,
    pub host: &'a Host,
    pub now: MonotonicTime,
    pub has_gpu: bool,
    pub window_server_may_show_high_dynamic_range: bool,
    pub nested_contexts: &'a dyn NestedContexts,
}

impl PresentingEnvironment<'_> {
    pub fn window_server_showing_context(&self) -> Option<&WindowServerObject> {
        self.remote_layer
            .as_ref()
            .map(|remote_layer| &*remote_layer.window_server)
    }
}

/// A frame drawn into a store, or for a frame that changes only what the window server shows above the frame on
/// screen, into nothing.
pub struct PreparedFrame {
    pub rendered_surface: Option<Rc<PaintingSurface>>,
    pub bitmap_id: i32,
    pub damage_rect: IntRect,
    pub device_pixel_ratio: f64,
    /// What the window server shows above the frame, bottom to top, lent by the presenting state until the next frame.
    pub sublayers: Vec<FfiSublayer>,
}

pub(crate) struct FrameToPaint<'a> {
    pub display_list: &'a Rc<DisplayList>,
    pub frame_inputs: &'a FrameInputs,
}

impl FrameToPaint<'_> {
    pub fn surface_clear_color(&self) -> Color {
        self.display_list.surface_clear_color().unwrap_or(Color::TRANSPARENT)
    }
}

/// Whether screenshots paint through the layers and tiles that frames are presented from, so that tests that compare
/// screenshots check that path.
fn screenshots_paint_from_layers() -> bool {
    static ENABLED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ENABLED.get_or_init(|| std::env::var_os("LADYBIRD_SCREENSHOTS_FROM_LAYERS").is_some_and(|value| !value.is_empty()))
}

/// The state of a context that painting a frame through its layers reads and changes, besides the frame itself.
struct LayeredPaintState {
    layers: ContextLayers,
    painted_frame: Option<PaintedFrame>,
    layer_overlay: LayerOverlaySwitch,
    viewport_size: IntSize,
    shown_in_remote_layer: bool,
    backing_stores: BackingStores,
}

impl LayeredPaintState {
    /// The state of a first frame of the screenshot's size that the window server and the presentation do not show:
    /// no layers, tiles or frame from before, and no overlay of the layers. Without published stores, the frame draws
    /// no LCD text, as a replay of the list into a screenshot does not.
    fn of_screenshot(target_size: IntSize) -> Self {
        Self {
            layers: ContextLayers::default(),
            painted_frame: None,
            layer_overlay: LayerOverlaySwitch::default(),
            viewport_size: target_size,
            shown_in_remote_layer: false,
            backing_stores: BackingStores::default(),
        }
    }
}

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
    const MAXIMUM_PIXEL_COUNT_OF_64_MIB_SURFACE: f32 = 16.0 * 1024.0 * 1024.0;
    scale.width = scale.width.min((MAXIMUM_DIMENSION - 1.0) / viewport_size.width as f32);
    scale.height = scale
        .height
        .min((MAXIMUM_DIMENSION - 1.0) / viewport_size.height as f32);
    let pixel_count = (viewport_size.width as f32 * scale.width) * (viewport_size.height as f32 * scale.height);
    const MAXIMUM_CONTENT_PIXELS: f32 = MAXIMUM_PIXEL_COUNT_OF_64_MIB_SURFACE - 2.0 * MAXIMUM_DIMENSION - 1.0;
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

fn size_of(rect: IntRect) -> IntSize {
    IntSize {
        width: rect.width,
        height: rect.height,
    }
}

fn inflated(rect: IntRect, margin: i32) -> IntRect {
    if rect.is_empty() || margin == 0 {
        return rect;
    }
    rect.inflated_edges(margin, margin, margin, margin)
}

pub struct ContextPresenting {
    pub presents_to_client: bool,
    pub display_id: Option<u64>,
    pub display_refresh_rate: f64,
    /// Whether the presentation shows this context's frames from CPU memory for now, as when the UI paints it itself.
    pub presents_from_cpu_memory: bool,
    pub scales_frames_to_fit: bool,
    pub shown_in_remote_layer: bool,
    /// The viewport size of the frame that the page presented last, which it laid out at.
    pub size_laid_out_by_page: IntSize,
    pub rendering_opportunity_request: PacedTickRequest,
    pub clock_tick_request: PacedTickRequest,

    viewport_size: IntSize,
    device_pixel_ratio: f64,
    window_resize_in_progress: bool,

    pending_present_frame: Option<PendingFrame>,
    pending_present_frame_scheduled: bool,
    presented_frame: Option<IntRect>,
    frame_generation: u64,

    frame_without_store_in_flight: bool,
    backing_stores: BackingStores,
    sublayers_of_latest_frame: Vec<FfiSublayer>,
    size_of_latest_rendered_frame: IntSize,
    store_damage: StoreDamage,
    surplus_store_retirement_is_scheduled: bool,
    // The context was hidden long enough to keep only the store that holds its latest frame, and gets no new stores
    // until it is shown again.
    stores_are_retired_while_hidden: bool,

    surface_drawn_by_parent: Option<Rc<PaintingSurface>>,
    unbounded_raster_scale_of_embedding: FloatSize,
    raster_scale: FloatSize,
    raster_translation: FloatPoint,

    painted_frame: Option<PaintedFrame>,
    layers: ContextLayers,
    layer_overlay: LayerOverlaySwitch,
}

impl ContextPresenting {
    pub fn new(presents_to_client: bool) -> Self {
        Self {
            presents_to_client,
            display_id: None,
            display_refresh_rate: 60.0,
            presents_from_cpu_memory: false,
            scales_frames_to_fit: false,
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
            frame_generation: 0,
            frame_without_store_in_flight: false,
            backing_stores: BackingStores::default(),
            sublayers_of_latest_frame: Vec::new(),
            size_of_latest_rendered_frame: IntSize::default(),
            store_damage: StoreDamage::default(),
            surplus_store_retirement_is_scheduled: false,
            stores_are_retired_while_hidden: false,
            surface_drawn_by_parent: None,
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
            layer_overlay: LayerOverlaySwitch::default(),
        }
    }

    pub fn store_publication(&self) -> StorePublication {
        self.backing_stores.publication()
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

    /// The viewport rect of the next frame, or of the frame presented last, at the viewport size.
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
        publication: StorePublication,
        gpu_sharing_allowed: bool,
        presentation_id: Option<u64>,
    ) -> StoreResizeOutcome {
        if !self.presents_to_client {
            self.fit_surface_drawn_by_parent_to_viewport();
            return StoreResizeOutcome::Unchanged;
        }
        if self.backing_stores.is_rendering() {
            return StoreResizeOutcome::Unchanged;
        }
        let Some(allocation) =
            self.backing_stores
                .resize_if_needed(self.raster_size(), self.window_resize_in_progress, publication)
        else {
            return StoreResizeOutcome::Unchanged;
        };
        let shares_gpu_images = env.has_gpu && gpu_sharing_allowed;
        let outcome =
            match self
                .backing_stores
                .allocate(allocation, publication, shares_gpu_images, &mut |kind, size| {
                    env.raster
                        .create_store(env.context_id, kind, size, env.window_server_showing_context())
                }) {
                AllocationOutcome::Offscreen => StoreResizeOutcome::ReallocatedOffscreen,
                AllocationOutcome::Failed => StoreResizeOutcome::Failed,
                AllocationOutcome::ForViewer(stores) => {
                    if publication == StorePublication::Published {
                        let presentation_id = presentation_id.expect("published stores have a presentation");
                        env.raster
                            .publish_stores(presentation_id, env.context_id, false, stores);
                    }
                    StoreResizeOutcome::ReallocatedForViewer
                }
            };
        if outcome != StoreResizeOutcome::Unchanged {
            self.painted_frame = None;
            self.store_damage.forget_every_store();
        }
        outcome
    }

    // A resize can arrive before the parent's next replay supplies the new embedding transform, so the existing
    // embedding's scale is bounded for the new viewport. Bounding the already bounded scale instead can round it
    // differently, which would discard the raster on every replay.
    fn fit_surface_drawn_by_parent_to_viewport(&mut self) {
        let scale = bounded_composited_raster_scale(self.unbounded_raster_scale_of_embedding, self.viewport_size);
        if self.raster_scale != scale {
            self.raster_scale = scale;
            self.painted_frame = None;
        }
        let raster_size = self.raster_size();
        if self
            .surface_drawn_by_parent
            .as_ref()
            .is_some_and(|surface| surface.size() != raster_size)
        {
            self.surface_drawn_by_parent = None;
        }
    }

    pub fn invalidate_backing_stores(&mut self) {
        self.backing_stores.invalidate();
        self.frame_without_store_in_flight = false;
    }

    pub fn wants_backing_store_for_pending_frame(&self, display_list: Option<&Rc<DisplayList>>) -> bool {
        self.presents_to_client && self.pending_present_frame.is_some() && self.can_render_frame(display_list)
    }

    /// Answers whether a store that was presented became available.
    pub fn release_presented_store(&mut self, bitmap_id: i32) -> bool {
        self.backing_stores.release_store(bitmap_id)
    }

    pub fn release_all_presented_stores(&mut self) {
        self.backing_stores.release_all_presented_stores();
    }

    /// The store the remote layer shows the latest frame from.
    pub fn window_server_surface_of_latest_rendered_store(&self) -> Option<&Rc<PresentableSurface>> {
        self.backing_stores.window_server_surface_of_latest_rendered_store()
    }

    fn publish_added_stores(
        raster: &RasterHost,
        context_id: CompositorContextId,
        presentation_id: Option<u64>,
        stores: Publication,
    ) {
        if let Some(presentation_id) = presentation_id
            && !stores.bitmap_ids.is_empty()
        {
            raster.publish_stores(presentation_id, context_id, true, stores);
        }
    }

    fn report_retired_stores(
        raster: &RasterHost,
        context_id: CompositorContextId,
        presentation_id: Option<u64>,
        bitmap_ids: &[i32],
    ) {
        if let Some(presentation_id) = presentation_id
            && !bitmap_ids.is_empty()
        {
            raster.retire_published_stores(presentation_id, context_id, bitmap_ids);
        }
    }

    /// Answers whether a store was added, which the presentation that shows the stores, if one does, is given.
    pub fn add_store_if_window_server_still_reads_every_released_store(
        &mut self,
        raster: &RasterHost,
        context_id: CompositorContextId,
        publishing_presentation: Option<u64>,
        window_server: Option<&WindowServerObject>,
    ) -> bool {
        let Some(stores) = self
            .backing_stores
            .add_store_if_window_server_still_reads_every_released_store(&mut |kind, size| {
                raster.create_store(context_id, kind, size, window_server)
            })
        else {
            return false;
        };
        Self::publish_added_stores(raster, context_id, publishing_presentation, stores);
        true
    }

    /// Answers whether surplus stores are left.
    pub fn retire_idle_surplus_stores(
        &mut self,
        raster: &RasterHost,
        context_id: CompositorContextId,
        publishing_presentation: Option<u64>,
    ) -> bool {
        let retired_bitmap_ids = self.backing_stores.retire_idle_surplus_stores();
        Self::report_retired_stores(raster, context_id, publishing_presentation, &retired_bitmap_ids);
        self.backing_stores.has_surplus_stores()
    }

    /// Retires every store but the one that holds the latest frame, and answers whether a store is left that can be
    /// retired once nothing renders into, shows or reads it.
    pub fn retire_stores_of_hidden_context(
        &mut self,
        raster: &RasterHost,
        context_id: CompositorContextId,
        publishing_presentation: Option<u64>,
    ) -> bool {
        let retired_bitmap_ids = self.backing_stores.retire_stores_except_the_latest_rendered();
        Self::report_retired_stores(raster, context_id, publishing_presentation, &retired_bitmap_ids);
        self.backing_stores.has_stores_left_to_retire_while_hidden()
    }

    /// Answers false where the stores could not be added back to the one kept, so that the context needs a whole new
    /// allocation.
    pub fn restore_stores_retired_while_hidden(
        &mut self,
        raster: &RasterHost,
        context_id: CompositorContextId,
        publishing_presentation: Option<u64>,
        window_server: Option<&WindowServerObject>,
    ) -> bool {
        let Some(stores) = self.backing_stores.add_stores_up_to_initial_count(&mut |kind, size| {
            raster.create_store(context_id, kind, size, window_server)
        }) else {
            return false;
        };
        Self::publish_added_stores(raster, context_id, publishing_presentation, stores);
        true
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

    pub fn schedule_hidden_store_retirement(&self, context_id: CompositorContextId, host: &Host) {
        host.set_timer(
            context_id,
            ContextTimer::HiddenStoreRetirement,
            TIME_HIDDEN_BEFORE_RETIRING_STORES,
        );
    }

    pub fn retry_hidden_store_retirement(&self, context_id: CompositorContextId, host: &Host) {
        host.set_timer(
            context_id,
            ContextTimer::HiddenStoreRetirement,
            HIDDEN_STORE_RETIREMENT_RETRY_DELAY,
        );
    }

    pub fn stores_are_retired_while_hidden(&self) -> bool {
        self.stores_are_retired_while_hidden
    }

    pub fn start_retiring_stores_while_hidden(&mut self) {
        self.stores_are_retired_while_hidden = true;
    }

    /// Answers whether the stores were retired while the context was hidden.
    pub fn stop_retiring_stores_as_shown(&mut self) -> bool {
        std::mem::take(&mut self.stores_are_retired_while_hidden)
    }

    pub fn did_finish_gpu_present(&mut self, bitmap_id: i32, frame_size: IntSize) {
        self.backing_stores.complete_rendering(bitmap_id);
        self.size_of_latest_rendered_frame = frame_size;
    }

    pub fn did_deliver_frame_without_store(&mut self) {
        self.frame_without_store_in_flight = false;
    }

    pub fn has_surface_drawn_by_parent(&self) -> bool {
        self.surface_drawn_by_parent.is_some()
    }

    /// Makes the next paint redo the raster where the embedding now draws it at another scale or phase.
    pub fn update_composited_raster_transform(&mut self, destination_rect: FloatRect, transform: &FloatMatrix4x4) {
        if self.presents_to_client || self.viewport_size.is_empty() || destination_rect.is_empty() {
            return;
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
            return;
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
                return;
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
            return;
        }
        self.raster_scale = scale;
        self.raster_translation = translation;
        self.painted_frame = None;
    }

    pub fn composited_surface(&self) -> Option<ResolvedCompositedContext> {
        let surface = self.surface_drawn_by_parent.as_ref()?;
        let painted_frame = self.painted_frame.as_ref()?;
        Some(ResolvedCompositedContext {
            surface: surface.as_ffi().cast_mut().cast(),
            content_rect: FloatRect::new(
                self.raster_translation.x,
                self.raster_translation.y,
                painted_frame.viewport_size.width as f32 * self.raster_scale.width,
                painted_frame.viewport_size.height as f32 * self.raster_scale.height,
            ),
        })
    }

    /// What a parent that draws the context tells its frames apart by: it changes with each frame queued, and each time
    /// a frame cannot be painted into the surface the parent draws.
    pub fn frame_generation(&self) -> u64 {
        self.frame_generation
    }

    pub fn queue_present_frame(&mut self, pending_frame: PendingFrame) {
        self.frame_generation += 1;
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

    pub fn can_schedule_pending_present_frame_if_unblocked(&self) -> bool {
        !self.is_present_blocked() && self.pending_present_frame.is_some() && !self.pending_present_frame_scheduled
    }

    pub fn take_pending_present_frame_if_unblocked(&mut self) -> Option<PendingFrame> {
        if self.is_present_blocked() {
            return None;
        }
        self.pending_present_frame_scheduled = false;
        self.pending_present_frame.take()
    }

    pub fn has_pending_present_frame(&self) -> bool {
        self.pending_present_frame.is_some()
    }

    pub fn needs_rasterization(&self) -> bool {
        self.pending_present_frame.is_some()
            || (self.presented_frame.is_some()
                && (self.surface_drawn_by_parent.is_none() || self.painted_frame.is_none()))
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

    fn is_present_blocked(&self) -> bool {
        self.backing_stores.is_rendering()
            || self.frame_without_store_in_flight
            || !self.backing_stores.has_available_store()
    }

    fn can_render_frame(&self, display_list: Option<&Rc<DisplayList>>) -> bool {
        display_list.is_some() && self.backing_stores.is_valid()
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

    fn raster_rect(&self) -> IntRect {
        rect_of_size(self.raster_size())
    }

    fn raster_transform(&self) -> FloatMatrix4x4 {
        raster_transform_of(self.raster_scale, self.raster_translation)
    }

    fn raster_rect_of(&self, rect: IntRect) -> IntRect {
        if rect.is_empty() {
            return IntRect::default();
        }
        let raster_rect = rect_of_size(self.raster_size());
        if rect.contains_rect(rect_of_size(self.viewport_size)) {
            return raster_rect;
        }
        let rect = rect.to_float();
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
        frame_inputs: Option<FrameInputs>,
    ) -> Option<PreparedFrame> {
        if self.is_present_blocked() {
            self.queue_present_frame(pending_frame);
            return None;
        }
        if !self.can_render_frame(display_list) {
            self.presented_frame = Some(pending_frame.viewport_rect);
            return None;
        }
        let display_list = display_list.expect("a frame that can render has a display list");
        let frame_inputs = frame_inputs.expect("a context with a display list has frame inputs");
        self.layer_overlay.switch_for_frame(
            frame_inputs
                .overlays
                .compositor_layers
                .as_ref()
                .filter(|_| self.presents_to_client),
        );
        let live_content = self.live_content(env, display_list);
        let frame = FrameToPaint {
            display_list,
            frame_inputs: &frame_inputs,
        };
        let mut frame_damage = self.frame_damage_for(env, &pending_frame, &frame, &live_content);
        let mut damage_rect = frame_damage.rect_in_raster_device_pixels;
        // NB: A changed content rect must still reach the client even if the pixels are identical.
        if damage_rect.is_empty() && self.presented_frame == Some(pending_frame.viewport_rect) {
            let only_media_shown_by_window_server_changed =
                frame_damage.layers.shown_by_window_server() && frame_damage.live_media_changed;
            self.remember_painted_frame(
                env,
                pending_frame.viewport_rect,
                display_list,
                frame_inputs,
                &live_content,
                frame_damage.layers,
            );
            let sublayers = self.sublayers_shown_by_window_server(&live_content);
            let media_shown_by_window_server_moved_resized_or_turned = sublayers != self.sublayers_of_latest_frame;
            if !only_media_shown_by_window_server_changed && !media_shown_by_window_server_moved_resized_or_turned {
                return None;
            }
            self.frame_without_store_in_flight = true;
            self.sublayers_of_latest_frame = sublayers.clone();
            return Some(PreparedFrame {
                rendered_surface: None,
                bitmap_id: -1,
                damage_rect: IntRect::default(),
                device_pixel_ratio: self.device_pixel_ratio,
                sublayers,
            });
        }
        let Some((bitmap_id, store)) = self.backing_stores.acquire_render_target() else {
            self.queue_present_frame(pending_frame);
            return None;
        };
        let store_rect = rect_of_size(store.size());
        let repaint_rect = self.store_damage.take(bitmap_id, damage_rect, store_rect);
        let painted_rect = self.paint_layered_frame(
            env,
            &store,
            &frame,
            repaint_rect,
            &mut frame_damage.layers,
            &live_content,
            true,
        );
        // The other stores miss all of what this one now holds.
        if painted_rect != repaint_rect {
            damage_rect = rect_of_size(IntSize {
                width: pending_frame.viewport_rect.width,
                height: pending_frame.viewport_rect.height,
            });
            self.store_damage.add_damage_to_other_stores(bitmap_id, painted_rect);
        }
        self.remember_painted_frame(
            env,
            pending_frame.viewport_rect,
            display_list,
            frame_inputs,
            &live_content,
            frame_damage.layers,
        );
        let sublayers = self.sublayers_shown_by_window_server(&live_content);
        self.sublayers_of_latest_frame = sublayers.clone();
        Some(PreparedFrame {
            rendered_surface: Some(store),
            bitmap_id,
            damage_rect,
            device_pixel_ratio: self.device_pixel_ratio,
            sublayers,
        })
    }

    /// Paints the pending frame, or the frame presented last, into the surface of a context that its parent draws.
    pub fn paint_frame_drawn_by_parent(
        &mut self,
        env: &PresentingEnvironment<'_>,
        display_list: Option<&Rc<DisplayList>>,
        frame_inputs: Option<FrameInputs>,
    ) {
        let Some(display_list) = display_list else {
            return;
        };
        let frame_inputs = frame_inputs.expect("a context with a display list has frame inputs");
        // What the parent draws of the frame can go into the parent's tiles, which the overlay must stay out of.
        self.layer_overlay.switch_for_frame(None);
        let pending_frame = match self.pending_present_frame {
            Some(pending_frame) => pending_frame,
            None => match self.presented_frame {
                Some(presented_frame) => PendingFrame::repainting_everything(presented_frame),
                None => return,
            },
        };
        // The surface comes first, so that a frame that cannot be painted does not work out its layers.
        let raster_size = self.raster_size();
        let (surface, holds_earlier_frame) = match self.surface_drawn_by_parent.take() {
            Some(surface) if surface.size() == raster_size => (surface, true),
            _ => {
                let Some(surface) = env.raster.create_surface(raster_size, None, false) else {
                    // The parent draws nothing of the context into the tiles it rasters now. A new generation makes
                    // the next frame of the parent key those tiles again, and so paint the context again.
                    self.frame_generation += 1;
                    return;
                };
                (surface, false)
            }
        };
        let live_content = self.live_content(env, display_list);
        let frame = FrameToPaint {
            display_list,
            frame_inputs: &frame_inputs,
        };
        let mut frame_damage = self.frame_damage_for(env, &pending_frame, &frame, &live_content);
        let repaint_rect = if holds_earlier_frame {
            frame_damage.rect_in_raster_device_pixels
        } else {
            rect_of_size(raster_size)
        };
        let _ = self.paint_layered_frame(
            env,
            &surface,
            &frame,
            repaint_rect,
            &mut frame_damage.layers,
            &live_content,
            true,
        );
        self.remember_painted_frame(
            env,
            pending_frame.viewport_rect,
            display_list,
            frame_inputs,
            &live_content,
            frame_damage.layers,
        );
        env.raster.flush(&surface);
        self.surface_drawn_by_parent = Some(surface);
        self.presented_frame = Some(pending_frame.viewport_rect);
        self.pending_present_frame = None;
        self.pending_present_frame_scheduled = false;
    }

    /// Paints a screenshot into the target, without the overlays of the browser and at the raster's own scale.
    pub fn paint_screenshot(
        &mut self,
        env: &PresentingEnvironment<'_>,
        target: &Rc<PaintingSurface>,
        display_list: &Rc<DisplayList>,
        frame_inputs: &FrameInputs,
    ) {
        let frame = FrameToPaint {
            display_list,
            frame_inputs,
        };
        if !screenshots_paint_from_layers() || !self.paint_screenshot_from_layers(env, target, &frame) {
            let save_count = env.raster.save(target);
            replay_into(env, target, &frame, ReplaySelection::default());
            self.paint_overlays(env, target, frame_inputs, false);
            env.raster.restore_to_count(target, save_count);
        }
        env.raster.flush(target);
    }

    /// Paints a screenshot as the first frame of a context that only the reader of the screenshot sees, through layers
    /// and tiles of its own. Answers false, and paints nothing, where the layers cannot paint the screenshot as a replay
    /// of the list paints it.
    fn paint_screenshot_from_layers(
        &mut self,
        env: &PresentingEnvironment<'_>,
        target: &Rc<PaintingSurface>,
        frame: &FrameToPaint<'_>,
    ) -> bool {
        // The layers are rastered under the raster transform, which a screenshot does not apply.
        let target_size = target.size();
        if target_size.is_empty() || self.raster_transform() != FloatMatrix4x4::identity() {
            return false;
        }
        self.with_layered_paint_state(LayeredPaintState::of_screenshot(target_size), |this| {
            let live_content = this.live_content(env, frame.display_list);
            let pending_frame = PendingFrame::repainting_everything(rect_of_size(target_size));
            let mut frame_damage = this.frame_damage_for(env, &pending_frame, frame, &live_content);
            this.paint_layered_frame(
                env,
                target,
                frame,
                frame_damage.rect_in_raster_device_pixels,
                &mut frame_damage.layers,
                &live_content,
                false,
            );
        });
        true
    }

    /// Runs `paint` with the state that painting through the layers reads and changes swapped for `state`, and then
    /// puts the context's own state back.
    fn with_layered_paint_state(&mut self, mut state: LayeredPaintState, paint: impl FnOnce(&mut Self)) {
        self.swap_layered_paint_state(&mut state);
        paint(self);
        self.swap_layered_paint_state(&mut state);
    }

    fn swap_layered_paint_state(&mut self, state: &mut LayeredPaintState) {
        // Each field is named, so a new field of the state does not compile until it is swapped too.
        let LayeredPaintState {
            layers,
            painted_frame,
            layer_overlay,
            viewport_size,
            shown_in_remote_layer,
            backing_stores,
        } = state;
        std::mem::swap(&mut self.layers, layers);
        std::mem::swap(&mut self.painted_frame, painted_frame);
        std::mem::swap(&mut self.layer_overlay, layer_overlay);
        std::mem::swap(&mut self.viewport_size, viewport_size);
        std::mem::swap(&mut self.shown_in_remote_layer, shown_in_remote_layer);
        std::mem::swap(&mut self.backing_stores, backing_stores);
    }

    /// The surface that holds the frame on screen as a whole, with what the window server shows above the store
    /// composited into it, and the size of the frame in it.
    pub fn presented_frame_for_snapshot(
        &self,
        env: &PresentingEnvironment<'_>,
    ) -> Option<(Rc<PaintingSurface>, IntSize)> {
        let source = self.surface_of_presented_frame_with_window_server_layers_composited_in(env)?;
        let source_size = source.size();
        let frame_size = IntSize {
            width: self.size_of_latest_rendered_frame.width.min(source_size.width),
            height: self.size_of_latest_rendered_frame.height.min(source_size.height),
        };
        if frame_size.is_empty() {
            return None;
        }
        Some((source, frame_size))
    }

    fn surface_of_presented_frame_with_window_server_layers_composited_in(
        &self,
        env: &PresentingEnvironment<'_>,
    ) -> Option<Rc<PaintingSurface>> {
        let mut source = self.backing_stores.latest_rendered_surface()?;
        if let Some(painted_frame) = self
            .painted_frame
            .as_ref()
            .filter(|painted_frame| painted_frame.layers.shown_by_window_server())
        {
            let source_size = source.size();
            let composited = env.raster.create_surface(source_size, Some(&source), false);
            if let Some(composited) = composited {
                let source_rect = rect_of_size(source_size);
                env.raster
                    .draw_surface(&composited, &source, source_rect, source_rect, SurfaceBlend::Source);
                let frame = FrameToPaint {
                    display_list: &painted_frame.display_list,
                    frame_inputs: &painted_frame.frame_inputs,
                };
                let snapshot_env = PresentingEnvironment {
                    nested_contexts: &NoCompositedContexts,
                    remote_layer: env.remote_layer.clone(),
                    ..*env
                };
                self.composite_layers_shown_by_window_server(
                    &snapshot_env,
                    &composited,
                    &frame,
                    source_rect,
                    &painted_frame.layers,
                );
                self.paint_overlays(&snapshot_env, &composited, &painted_frame.frame_inputs, true);
                source = composited;
            }
        }
        Some(source)
    }

    pub fn spare_tile_eviction_timer_fired(&mut self, env: &PresentingEnvironment<'_>) {
        self.layers.spare_tile_eviction_timer_fired(env);
    }

    pub fn tile_raster_scratch_surface_release_timer_fired(&mut self) {
        self.layers.tile_raster_scratch_surface_release_timer_fired();
    }

    fn apply_raster_transform(&self, raster: &RasterHost, surface: &Rc<PaintingSurface>) {
        apply_raster_transform(raster, surface, self.raster_scale, self.raster_translation);
    }

    fn allows_lcd_text(&self, display_list: &DisplayList, frame_inputs: &FrameInputs) -> bool {
        let tree = frame_inputs.visual_context_tree.as_arc();
        // LCD coverage follows the physical pixel grid. Destination blending can erase pixels in the canvas fill.
        self.backing_stores.publication() == StorePublication::Published
            && !self.scales_frames_to_fit
            && self.device_pixel_ratio.fract() == 0.0
            && display_list.storage().summary().destination_blending_records.is_empty()
            // A blend or backdrop filter that reads the root group needs its original isolation from the canvas fill.
            && !tree.blend_mode_or_backdrop_filter_reads_root_group()
    }

    /// Paints what the compositor draws over the page: its scrollbars, and unless they are left out, the overlays of the
    /// browser and of the layers.
    fn paint_overlays(
        &self,
        env: &PresentingEnvironment<'_>,
        target: &Rc<PaintingSurface>,
        frame_inputs: &FrameInputs,
        paints_ui_overlays: bool,
    ) {
        env.raster
            .paint(env.context_id, target, PaintingResources::Overlays, false, |painter| {
                frame_inputs.overlays.paint(painter, paints_ui_overlays);
            });
        let Some(layer_overlay_frame) = self.layer_overlay.latest_frame().filter(|_| paints_ui_overlays) else {
            return;
        };
        env.raster
            .paint(env.context_id, target, PaintingResources::Overlays, false, |painter| {
                layer_overlay_frame.records.paint(painter);
            });
    }

    /// Until when the compositor layers overlay needs frames without anything else changing.
    pub fn layer_overlay_needs_frames_until(&self) -> Option<MonotonicTime> {
        self.layer_overlay.needs_frames_until()
    }

    /// What the compositor layers window is to be told of the frame painted last, if anything.
    pub fn take_description_of_layers_to_publish(&mut self, now: MonotonicTime) -> Option<CompositorLayersDescription> {
        self.layer_overlay.overlay_mut()?.take_description_to_publish(now)
    }
}

fn raster_transform_of(scale: FloatSize, translation: FloatPoint) -> FloatMatrix4x4 {
    FloatMatrix4x4 {
        elements: [
            [scale.width, 0.0, 0.0, translation.x],
            [0.0, scale.height, 0.0, translation.y],
            [0.0, 0.0, 1.0, 0.0],
            [0.0, 0.0, 0.0, 1.0],
        ],
    }
}

pub(crate) fn apply_raster_transform(
    raster: &RasterHost,
    surface: &Rc<PaintingSurface>,
    scale: FloatSize,
    translation: FloatPoint,
) {
    raster.translate(surface, translation.x, translation.y);
    raster.scale(surface, scale.width, scale.height);
}

pub(crate) fn replay_into(
    env: &PresentingEnvironment<'_>,
    target: &Rc<PaintingSurface>,
    frame: &FrameToPaint<'_>,
    selection: ReplaySelection<'_>,
) {
    let scroll_offsets = frame.frame_inputs.presented_scroll_state_snapshot.device_offsets();
    replay_with_scroll_offsets(env, target, frame, selection, scroll_offsets);
}

pub(crate) fn replay_with_scroll_offsets(
    env: &PresentingEnvironment<'_>,
    target: &Rc<PaintingSurface>,
    frame: &FrameToPaint<'_>,
    selection: ReplaySelection<'_>,
    scroll_offsets: &[FloatPoint],
) {
    let frame_inputs = frame.frame_inputs;
    let display_list = frame.display_list;
    assert_eq!(
        display_list.compatible_visual_context_tree_structural_epoch(),
        frame_inputs.visual_context_tree.structural_epoch()
    );
    env.raster.paint(
        env.context_id,
        target,
        PaintingResources::Page,
        selection.allows_lcd_text,
        |painter| {
            let scrollbars = frame_inputs.scrollbars_shown_for_the_page();
            let replay_frame = ReplayFrame {
                scroll_offsets,
                scrollbars: &scrollbars,
                time_ns: frame_inputs.painted_at.nanoseconds(),
                composited_contexts: env.nested_contexts,
            };
            let storage = display_list.storage();
            let tree = frame_inputs.visual_context_tree.as_arc();
            replay_display_list(
                tree,
                storage,
                display_list.root_group_color(),
                &replay_frame,
                selection,
                painter,
            );
        },
    );
}

/// Composites rasters of the frame into the target through `composite`, which enters the effects the compositor applies
/// to them as a replay of the frame's list enters them.
pub(crate) fn composite_inside_effects_of_frame(
    env: &PresentingEnvironment<'_>,
    target: &Rc<PaintingSurface>,
    frame: &FrameToPaint<'_>,
    composite: impl FnOnce(&mut CompositingInsideEffects<'_, '_, FfiRasterPainter>),
) {
    let frame_inputs = frame.frame_inputs;
    let display_list = frame.display_list;
    env.raster
        .paint(env.context_id, target, PaintingResources::Page, false, |painter| {
            let scrollbars = frame_inputs.scrollbars_shown_for_the_page();
            let replay_frame = ReplayFrame {
                scroll_offsets: frame_inputs.presented_scroll_state_snapshot.device_offsets(),
                scrollbars: &scrollbars,
                time_ns: frame_inputs.painted_at.nanoseconds(),
                composited_contexts: env.nested_contexts,
            };
            let storage = display_list.storage();
            let tree = frame_inputs.visual_context_tree.as_arc();
            composite_inside_effects(
                tree,
                storage,
                display_list.root_group_color(),
                &replay_frame,
                painter,
                composite,
            );
        });
}
