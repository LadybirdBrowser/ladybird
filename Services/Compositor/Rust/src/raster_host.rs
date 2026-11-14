/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The mechanism that presenting a frame drives in the C++ around the core: Skia surfaces and the painter, the backing
//! stores of each context, the surfaces the window server shows, the media that live content comes from, GPU
//! submission, and the display ticks. None of these calls comes back into the core before it returns.

use libcompositing_rust::display_list::commands::CompositorContextId;
use libcompositing_rust::display_list::storage::{DisplayListStorage, receive_packed_from_shared_memory};
use libcompositing_rust::host::FfiRasterPainter;
use libcompositing_rust::ipc_types::Core_AnonymousBuffer;
use libcompositing_rust::time::MonotonicTime;
use libgfx_rust::{Color, FloatRect, IntRect, IntSize, ScalingMode};
use std::collections::HashMap;
use std::ffi::c_void;
use std::ptr::NonNull;
use std::sync::Arc;

/// How a surface is drawn into another.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum SurfaceBlend {
    /// Replaces what the target holds.
    Source,
    SourceOver,
}

/// Whose resources a painting draws with.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum PaintingResources {
    Page,
    Overlays,
}

/// Who shows the backing stores of a context.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum StorePublication {
    #[default]
    Unpublished,
    /// The presentation shows the stores, and releases each when it shows another.
    Published,
    /// The compositor shows the stores itself, in a remote layer, so a store is replaced on screen by the next one.
    ShownInRemoteLayer,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum StoreResizeOutcome {
    Unchanged,
    /// The stores were reallocated, and nothing that shows them was given them.
    ReallocatedOffscreen,
    /// The presentation or the remote layer that shows the stores was given the new ones.
    ReallocatedForViewer,
    /// The stores of a remote layer could not be allocated.
    Failed,
}

/// When a frame can be handed to whoever shows it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FrameHandOff {
    OnceGpuWorkIsScheduled,
    OnceGpuWorkIsFinished,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiStoreState {
    pub is_valid: bool,
    pub is_rendering: bool,
    pub has_available_store: bool,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiStoreResize {
    pub raster_size: IntSize,
    pub window_resize_in_progress: bool,
    pub publication: StorePublication,
    pub gpu_sharing_allowed: bool,
    pub has_presentation: bool,
    pub presentation_id: u64,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiAcquiredStore {
    pub bitmap_id: i32,
    pub surface: *mut c_void,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiCanvasContent {
    pub id: u64,
    pub content_generation: u64,
    // Whether the window server can show the canvas as it is.
    pub has_presentable_surface: bool,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiVideoSinkContent {
    pub id: u64,
    pub has_frame: bool,
    pub frame_id: u64,
    pub frame_holds_high_dynamic_range: bool,
    // Whether the window server can show the frame as it is.
    pub frame_has_surface: bool,
}

/// A video sink that a display list draws: its resource ID, and the handle of the sink.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiVideoSinkResource {
    pub id: u64,
    pub sink_handle: u64,
}

/// The resources of a context that a display list or a visual context tree it accepted adds or removes, apart from the
/// fonts and image frames that went ahead of it.
#[repr(C)]
pub struct FfiResourceChanges<'a> {
    pub video_sinks: ak::Span<'a, FfiVideoSinkResource>,
    pub font_ids_to_remove: ak::Span<'a, u64>,
    pub image_frame_ids_to_remove: ak::Span<'a, u64>,
    pub video_sink_ids_to_remove: ak::Span<'a, u64>,
    pub display_list_ids_to_remove: ak::Span<'a, u64>,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiImageFrameContent {
    pub id: u64,
    pub pixel_generation: u64,
}

/// The live content of a context as it is now: the canvases asked about, every video sink and every image frame the
/// context's resources hold.
#[repr(C)]
pub struct FfiLiveContent<'a> {
    pub canvases: ak::Span<'a, FfiCanvasContent>,
    pub video_sinks: ak::Span<'a, FfiVideoSinkContent>,
    pub image_frames: ak::Span<'a, FfiImageFrameContent>,
}

/// The live content of a context as it is now, by ID: the canvases asked about, every video sink, and the pixel
/// generation of every image frame.
#[derive(Clone, Default, Debug)]
pub struct LiveContent {
    pub canvases: HashMap<u64, FfiCanvasContent>,
    pub video_sinks: HashMap<u64, FfiVideoSinkContent>,
    pub image_frames: HashMap<u64, u64>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum MediaKind {
    Canvas,
    Video,
}

/// Where the window server shows a sublayer, and how it clips it, in device pixels.
#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiSublayerPlacement {
    pub content_rect: IntRect,
    /// The a, b, c, d, e and f of an affine transform.
    pub content_to_frame: [f32; 6],
    pub clip_rect: FloatRect,
    pub has_rounded_clip: bool,
    pub rounded_clip_rect: FloatRect,
    pub rounded_clip_radius: f32,
    pub rounds_top_left: bool,
    pub rounds_top_right: bool,
    pub rounds_bottom_right: bool,
    pub rounds_bottom_left: bool,
    pub scaling_mode: ScalingMode,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum SublayerContentKind {
    Canvas,
    Video,
    PresentableSurface,
}

/// A sublayer the window server shows above a frame: a canvas or video of the context by its id, or a surface.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiSublayer {
    pub identity: u64,
    pub content_kind: SublayerContentKind,
    pub media_id: u64,
    pub presentable_surface: *mut c_void,
    pub placement: FfiSublayerPlacement,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiRemoteLayerFrame {
    /// Without the store, the frame shown before stays, and only the sublayers change.
    pub uses_store: bool,
    pub painted_size: IntSize,
    pub device_pixel_ratio: f64,
    /// Whether the page laid out the frame at the size it is painted at.
    pub laid_out_by_page: bool,
}

/// The entry points of the mechanism the core drives, and the object they are called on:
/// `Compositor::RustFFI::FfiRasterHost`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiRasterHost {
    pub host: *mut c_void,

    pub surface_retain: unsafe extern "C" fn(*mut c_void),
    pub surface_release: unsafe extern "C" fn(*mut c_void),
    /// Writes the size of the surface. NB: A C++ function returns Gfx::IntSize in memory rather than in a register
    ///     on Windows, so the size does not come back as the return value.
    pub surface_size: unsafe extern "C" fn(*mut c_void, *mut IntSize),
    /// Null where the surface could not be made. The surface is made like the one given, or for the GPU the compositor
    /// renders with where none is given.
    pub create_surface: unsafe extern "C" fn(*mut c_void, IntSize, *mut c_void) -> *mut c_void,
    pub surfaces_share_backend: unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool,
    pub surface_uses_gpu: unsafe extern "C" fn(*mut c_void) -> bool,
    pub surface_save: unsafe extern "C" fn(*mut c_void) -> i32,
    pub surface_restore_to_count: unsafe extern "C" fn(*mut c_void, i32),
    pub surface_translate: unsafe extern "C" fn(*mut c_void, f32, f32),
    pub surface_scale: unsafe extern "C" fn(*mut c_void, f32, f32),
    pub surface_clip_rect: unsafe extern "C" fn(*mut c_void, IntRect),
    pub surface_clear: unsafe extern "C" fn(*mut c_void, Color),
    /// Arguments: the target, the source, the rect of the source drawn, the rect of the target it is drawn into, and
    /// how it is blended.
    pub surface_draw_surface: unsafe extern "C" fn(*mut c_void, *mut c_void, IntRect, IntRect, SurfaceBlend),
    pub flush_surface: unsafe extern "C" fn(*mut c_void, *mut c_void),

    /// Arguments: the context, the surface, whose resources it draws with, and where the painter is written. The
    /// painting lasts until `end_painting`, and paintings nest.
    pub begin_painting: unsafe extern "C" fn(*mut c_void, u64, *mut c_void, PaintingResources, *mut FfiRasterPainter),
    pub end_painting: unsafe extern "C" fn(*mut c_void),
    /// Lets the raster caches of the context's page drop what the frames before the one that starts did not use.
    pub start_raster_cache_frame: unsafe extern "C" fn(*mut c_void, u64),

    pub store_state: unsafe extern "C" fn(*mut c_void, u64) -> FfiStoreState,
    pub resize_stores: unsafe extern "C" fn(*mut c_void, u64, *const FfiStoreResize) -> StoreResizeOutcome,
    pub invalidate_stores: unsafe extern "C" fn(*mut c_void, u64),
    pub acquire_store: unsafe extern "C" fn(*mut c_void, u64, *mut FfiAcquiredStore) -> bool,
    pub complete_rendering: unsafe extern "C" fn(*mut c_void, u64, i32),
    pub release_store: unsafe extern "C" fn(*mut c_void, u64, i32) -> bool,
    pub release_all_presented_stores: unsafe extern "C" fn(*mut c_void, u64),
    /// Lent; null before any store was rendered into.
    pub latest_rendered_store_surface: unsafe extern "C" fn(*mut c_void, u64) -> *mut c_void,
    /// Arguments: the context, and the presentation that shows its stores, if it has one. Answers whether a store was
    /// added.
    pub add_store_if_window_server_reads_every_released_store:
        unsafe extern "C" fn(*mut c_void, u64, bool, u64) -> bool,
    /// The same arguments. Answers whether surplus stores are left.
    pub retire_idle_surplus_stores: unsafe extern "C" fn(*mut c_void, u64, bool, u64) -> bool,

    /// Arguments: the context, the pool, the size, and a surface the new one is made like. Null where none could be had.
    pub next_presentable_surface: unsafe extern "C" fn(*mut c_void, u64, u32, IntSize, *mut c_void) -> *mut c_void,
    pub drop_presentable_surface_pool: unsafe extern "C" fn(*mut c_void, u64, u32),
    pub presentable_surface_retain: unsafe extern "C" fn(*mut c_void),
    pub presentable_surface_release: unsafe extern "C" fn(*mut c_void),
    /// Lent for as long as the presentable surface lives.
    pub presentable_surface_painting_surface: unsafe extern "C" fn(*mut c_void) -> *mut c_void,

    pub live_content: unsafe extern "C" fn(
        *mut c_void,
        u64,
        ak::Span<'_, u64>,
        *mut c_void,
        unsafe extern "C" fn(*mut c_void, *const FfiLiveContent<'_>),
    ),
    pub remote_layer_can_show:
        unsafe extern "C" fn(*mut c_void, u64, MediaKind, u64, *const FfiSublayerPlacement, f64) -> bool,
    pub remote_layer_can_show_high_dynamic_range: unsafe extern "C" fn(*mut c_void, u64) -> bool,
    /// Resolves the content of the sublayers, which the window server then holds busy until the list is destroyed or
    /// shown.
    pub build_sublayers: unsafe extern "C" fn(*mut c_void, u64, ak::Span<'_, FfiSublayer>) -> *mut c_void,
    pub destroy_sublayers: unsafe extern "C" fn(*mut c_void),

    pub frame_hand_off: unsafe extern "C" fn(*mut c_void, u64) -> FrameHandOff,
    /// Submits the GPU work drawn into the surface, or into the surfaces the window server shows where it is null,
    /// answering a token that `compositor_core_gpu_work_finished` hands back once it is done, on a later turn.
    pub submit_frame: unsafe extern "C" fn(*mut c_void, u64, *mut c_void) -> u64,
    /// Calls `compositor_core_frame_ready_for_hand_off` with the token on a later turn, once the work is scheduled.
    pub hand_off_once_gpu_work_is_scheduled: unsafe extern "C" fn(*mut c_void, u64),
    /// Shows a frame in the remote layer that shows the context, with the sublayers given, which it takes over. Answers
    /// false where the layer could not show it, after which the context is no longer shown in it. Writes the
    /// presentation the layer is in.
    pub show_frame_in_remote_layer:
        unsafe extern "C" fn(*mut c_void, u64, *const FfiRemoteLayerFrame, *mut c_void, *mut u64) -> bool,
    pub stop_showing_in_remote_layer: unsafe extern "C" fn(*mut c_void, u64),
    pub log_presented_frame: unsafe extern "C" fn(*mut c_void, u64, IntRect),
    /// Calls `compositor_core_presented_store_released` on a later turn.
    pub release_store_later: unsafe extern "C" fn(*mut c_void, u64, i32),

    /// Arguments: whether the display is known, the display, and its refresh rate.
    pub request_vsync: unsafe extern "C" fn(*mut c_void, bool, u64, f64),
    pub most_recent_tick_time: unsafe extern "C" fn(*mut c_void, bool, u64, i64, f64, *mut i64) -> bool,
    /// Updates every video sink, and hands the receiver the contexts that draw a sink with a new frame.
    pub update_video_sinks:
        unsafe extern "C" fn(*mut c_void, *mut c_void, unsafe extern "C" fn(*mut c_void, ak::Span<'_, u64>)),
    pub context_video_sinks_require_updates: unsafe extern "C" fn(*mut c_void, u64) -> bool,
    pub set_maximum_display_refresh_rate: unsafe extern "C" fn(*mut c_void, f64),

    /// Maps the shared memory of the descriptor, which it takes over, answering a mapping that `unmap_shared_memory`
    /// drops, or null where the descriptor does not hold that much. Writes where the memory is and its size.
    pub map_shared_memory: unsafe extern "C" fn(*mut c_void, i32, u64, *mut *const u8, *mut usize) -> *mut c_void,
    pub unmap_shared_memory: unsafe extern "C" fn(*mut c_void),
    pub apply_resource_changes: unsafe extern "C" fn(*mut c_void, u64, *const FfiResourceChanges<'_>),
    /// Arguments: the client, the token it staged the rasterization under, and the fonts and image frames the display
    /// list draws. Answers whether the rasterization is staged with all of them. A video sink drawn without a handle
    /// draws nothing, so the sinks are not asked about.
    pub staged_rasterization_has_resources:
        unsafe extern "C" fn(*mut c_void, u64, u64, ak::Span<'_, u64>, ak::Span<'_, u64>) -> bool,
    /// Writes the painter that paints into the target of the staged rasterization, with its resources, until
    /// `finish_staged_rasterization`. Answers false for a target that cannot be painted into.
    pub begin_staged_rasterization: unsafe extern "C" fn(*mut c_void, u64, u64, *mut FfiRasterPainter) -> bool,
    /// Ends the painting that began, flushes it into the target, and drops what was staged.
    pub finish_staged_rasterization: unsafe extern "C" fn(*mut c_void, u64, u64),
}

/// A surface of the C++ side, which this keeps alive.
pub struct SurfaceHandle {
    surface: NonNull<c_void>,
    release: unsafe extern "C" fn(*mut c_void),
    retain: unsafe extern "C" fn(*mut c_void),
}

impl SurfaceHandle {
    pub fn as_ptr(&self) -> *mut c_void {
        self.surface.as_ptr()
    }
}

impl Clone for SurfaceHandle {
    fn clone(&self) -> Self {
        // SAFETY: The surface is live, as this holds it.
        unsafe { (self.retain)(self.surface.as_ptr()) };
        Self {
            surface: self.surface,
            release: self.release,
            retain: self.retain,
        }
    }
}

impl Drop for SurfaceHandle {
    fn drop(&mut self) {
        // SAFETY: This holds the reference it gives back.
        unsafe { (self.release)(self.surface.as_ptr()) };
    }
}

/// A surface the window server shows, which this keeps alive.
pub struct PresentableSurfaceHandle {
    surface: NonNull<c_void>,
    release: unsafe extern "C" fn(*mut c_void),
}

impl PresentableSurfaceHandle {
    pub fn as_ptr(&self) -> *mut c_void {
        self.surface.as_ptr()
    }
}

impl Drop for PresentableSurfaceHandle {
    fn drop(&mut self) {
        // SAFETY: This holds the reference it gives back.
        unsafe { (self.release)(self.surface.as_ptr()) };
    }
}

/// Sublayers whose content the C++ side resolved, which keep that content busy until they are shown or dropped.
pub struct SublayersHandle {
    sublayers: NonNull<c_void>,
    destroy: unsafe extern "C" fn(*mut c_void),
}

impl SublayersHandle {
    pub fn into_raw(self) -> *mut c_void {
        let sublayers = self.sublayers.as_ptr();
        std::mem::forget(self);
        sublayers
    }
}

impl Drop for SublayersHandle {
    fn drop(&mut self) {
        // SAFETY: This owns the sublayers.
        unsafe { (self.destroy)(self.sublayers.as_ptr()) };
    }
}

pub struct RasterHost(FfiRasterHost);

impl RasterHost {
    /// # Safety
    ///
    /// `ffi.host` must outlive the `RasterHost`, and none of its entry points may call back into the core.
    pub unsafe fn new(ffi: FfiRasterHost) -> Self {
        Self(ffi)
    }

    fn host(&self) -> *mut c_void {
        self.0.host
    }

    /// Takes a reference to a surface the C++ side lends.
    pub fn retain_surface(&self, surface: *mut c_void) -> Option<SurfaceHandle> {
        let surface = NonNull::new(surface)?;
        // SAFETY: The C++ side lends a live surface.
        unsafe { (self.0.surface_retain)(surface.as_ptr()) };
        Some(SurfaceHandle {
            surface,
            release: self.0.surface_release,
            retain: self.0.surface_retain,
        })
    }

    /// Hands the reference the handle holds over to the C++ side.
    pub fn leak_surface(&self, surface: SurfaceHandle) -> *mut c_void {
        let pointer = surface.as_ptr();
        std::mem::forget(surface);
        pointer
    }

    // Adopts the reference the C++ side handed over with a new surface.
    fn adopt_surface(&self, surface: *mut c_void) -> Option<SurfaceHandle> {
        Some(SurfaceHandle {
            surface: NonNull::new(surface)?,
            release: self.0.surface_release,
            retain: self.0.surface_retain,
        })
    }

    pub fn surface_size(&self, surface: &SurfaceHandle) -> IntSize {
        let mut size = IntSize::default();
        // SAFETY: The surface is live, and the host writes the size during the call.
        unsafe { (self.0.surface_size)(surface.as_ptr(), &raw mut size) };
        size
    }

    pub fn create_surface(&self, size: IntSize, like: Option<&SurfaceHandle>) -> Option<SurfaceHandle> {
        let like = like.map_or(std::ptr::null_mut(), SurfaceHandle::as_ptr);
        // SAFETY: The host outlives this, and the surface it is made like is live.
        self.adopt_surface(unsafe { (self.0.create_surface)(self.host(), size, like) })
    }

    pub fn surfaces_share_backend(&self, a: &SurfaceHandle, b: &SurfaceHandle) -> bool {
        // SAFETY: Both surfaces are live.
        unsafe { (self.0.surfaces_share_backend)(a.as_ptr(), b.as_ptr()) }
    }

    pub fn surface_uses_gpu(&self, surface: &SurfaceHandle) -> bool {
        // SAFETY: The surface is live.
        unsafe { (self.0.surface_uses_gpu)(surface.as_ptr()) }
    }

    pub fn start_raster_cache_frame(&self, context_id: CompositorContextId) {
        // SAFETY: The host outlives this.
        unsafe { (self.0.start_raster_cache_frame)(self.host(), context_id.0) };
    }

    pub fn save(&self, surface: &SurfaceHandle) -> i32 {
        // SAFETY: The surface is live.
        unsafe { (self.0.surface_save)(surface.as_ptr()) }
    }

    pub fn restore_to_count(&self, surface: &SurfaceHandle, count: i32) {
        // SAFETY: The surface is live.
        unsafe { (self.0.surface_restore_to_count)(surface.as_ptr(), count) };
    }

    pub fn translate(&self, surface: &SurfaceHandle, x: f32, y: f32) {
        // SAFETY: The surface is live.
        unsafe { (self.0.surface_translate)(surface.as_ptr(), x, y) };
    }

    pub fn scale(&self, surface: &SurfaceHandle, x: f32, y: f32) {
        // SAFETY: The surface is live.
        unsafe { (self.0.surface_scale)(surface.as_ptr(), x, y) };
    }

    pub fn clip_rect(&self, surface: &SurfaceHandle, rect: IntRect) {
        // SAFETY: The surface is live.
        unsafe { (self.0.surface_clip_rect)(surface.as_ptr(), rect) };
    }

    pub fn clear(&self, surface: &SurfaceHandle, color: Color) {
        // SAFETY: The surface is live.
        unsafe { (self.0.surface_clear)(surface.as_ptr(), color) };
    }

    pub fn draw_surface(
        &self,
        target: &SurfaceHandle,
        source: &SurfaceHandle,
        source_rect: IntRect,
        target_rect: IntRect,
        blend: SurfaceBlend,
    ) {
        // SAFETY: Both surfaces are live.
        unsafe { (self.0.surface_draw_surface)(target.as_ptr(), source.as_ptr(), source_rect, target_rect, blend) };
    }

    pub fn flush(&self, surface: &SurfaceHandle) {
        // SAFETY: The host outlives this, and the surface is live.
        unsafe { (self.0.flush_surface)(self.host(), surface.as_ptr()) };
    }

    /// Paints into the surface through a painter, which `paint` drives.
    pub fn paint(
        &self,
        context_id: CompositorContextId,
        surface: &SurfaceHandle,
        resources: PaintingResources,
        paint: impl FnOnce(&mut FfiRasterPainter),
    ) {
        let mut painter = std::mem::MaybeUninit::<FfiRasterPainter>::uninit();
        // SAFETY: The host outlives this, the surface is live, and the host writes the painter.
        let mut painter = unsafe {
            (self.0.begin_painting)(
                self.host(),
                context_id.0,
                surface.as_ptr(),
                resources,
                painter.as_mut_ptr(),
            );
            painter.assume_init()
        };
        paint(&mut painter);
        // SAFETY: The host outlives this, and the painting began above.
        unsafe { (self.0.end_painting)(self.host()) };
    }

    pub fn store_state(&self, context_id: CompositorContextId) -> FfiStoreState {
        // SAFETY: The host outlives this.
        unsafe { (self.0.store_state)(self.host(), context_id.0) }
    }

    pub fn resize_stores(&self, context_id: CompositorContextId, resize: &FfiStoreResize) -> StoreResizeOutcome {
        // SAFETY: The host outlives this.
        unsafe { (self.0.resize_stores)(self.host(), context_id.0, resize) }
    }

    pub fn invalidate_stores(&self, context_id: CompositorContextId) {
        // SAFETY: The host outlives this.
        unsafe { (self.0.invalidate_stores)(self.host(), context_id.0) };
    }

    pub fn acquire_store(&self, context_id: CompositorContextId) -> Option<(i32, SurfaceHandle)> {
        let mut acquired = FfiAcquiredStore {
            bitmap_id: -1,
            surface: std::ptr::null_mut(),
        };
        // SAFETY: The host outlives this.
        if !unsafe { (self.0.acquire_store)(self.host(), context_id.0, &raw mut acquired) } {
            return None;
        }
        Some((acquired.bitmap_id, self.retain_surface(acquired.surface)?))
    }

    pub fn complete_rendering(&self, context_id: CompositorContextId, bitmap_id: i32) {
        // SAFETY: The host outlives this.
        unsafe { (self.0.complete_rendering)(self.host(), context_id.0, bitmap_id) };
    }

    pub fn release_store(&self, context_id: CompositorContextId, bitmap_id: i32) -> bool {
        // SAFETY: The host outlives this.
        unsafe { (self.0.release_store)(self.host(), context_id.0, bitmap_id) }
    }

    pub fn release_all_presented_stores(&self, context_id: CompositorContextId) {
        // SAFETY: The host outlives this.
        unsafe { (self.0.release_all_presented_stores)(self.host(), context_id.0) };
    }

    pub fn latest_rendered_store_surface(&self, context_id: CompositorContextId) -> Option<SurfaceHandle> {
        // SAFETY: The host outlives this.
        self.retain_surface(unsafe { (self.0.latest_rendered_store_surface)(self.host(), context_id.0) })
    }

    pub fn add_store_if_window_server_reads_every_released_store(
        &self,
        context_id: CompositorContextId,
        presentation_id: Option<u64>,
    ) -> bool {
        // SAFETY: The host outlives this.
        unsafe {
            (self.0.add_store_if_window_server_reads_every_released_store)(
                self.host(),
                context_id.0,
                presentation_id.is_some(),
                presentation_id.unwrap_or_default(),
            )
        }
    }

    pub fn retire_idle_surplus_stores(&self, context_id: CompositorContextId, presentation_id: Option<u64>) -> bool {
        // SAFETY: The host outlives this.
        unsafe {
            (self.0.retire_idle_surplus_stores)(
                self.host(),
                context_id.0,
                presentation_id.is_some(),
                presentation_id.unwrap_or_default(),
            )
        }
    }

    pub fn next_presentable_surface(
        &self,
        context_id: CompositorContextId,
        pool: u32,
        size: IntSize,
        like: &SurfaceHandle,
    ) -> Option<PresentableSurfaceHandle> {
        // SAFETY: The host outlives this, and the surface is live.
        let surface =
            unsafe { (self.0.next_presentable_surface)(self.host(), context_id.0, pool, size, like.as_ptr()) };
        Some(PresentableSurfaceHandle {
            surface: NonNull::new(surface)?,
            release: self.0.presentable_surface_release,
        })
    }

    pub fn drop_presentable_surface_pool(&self, context_id: CompositorContextId, pool: u32) {
        // SAFETY: The host outlives this.
        unsafe { (self.0.drop_presentable_surface_pool)(self.host(), context_id.0, pool) };
    }

    pub fn painting_surface_of(&self, surface: &PresentableSurfaceHandle) -> SurfaceHandle {
        // SAFETY: The presentable surface is live, and lends its painting surface.
        self.retain_surface(unsafe { (self.0.presentable_surface_painting_surface)(surface.as_ptr()) })
            .expect("a presentable surface has a painting surface")
    }

    pub fn live_content(&self, context_id: CompositorContextId, canvas_ids: &[u64]) -> LiveContent {
        unsafe extern "C" fn receive(receiver: *mut c_void, content: *const FfiLiveContent<'_>) {
            // SAFETY: The receiver is the content below, and the host lends what it found for the call.
            let (receiver, content) = unsafe { (&mut *receiver.cast::<LiveContent>(), &*content) };
            receiver.canvases = content
                .canvases
                .as_slice()
                .iter()
                .map(|canvas| (canvas.id, *canvas))
                .collect();
            receiver.video_sinks = content
                .video_sinks
                .as_slice()
                .iter()
                .map(|sink| (sink.id, *sink))
                .collect();
            receiver.image_frames = content
                .image_frames
                .as_slice()
                .iter()
                .map(|image| (image.id, image.pixel_generation))
                .collect();
        }
        let mut content = LiveContent::default();
        // SAFETY: The host outlives this, and answers synchronously.
        unsafe {
            (self.0.live_content)(
                self.host(),
                context_id.0,
                ak::Span::new(canvas_ids),
                (&raw mut content).cast(),
                receive,
            );
        }
        content
    }

    pub fn remote_layer_can_show(
        &self,
        context_id: CompositorContextId,
        kind: MediaKind,
        id: u64,
        placement: &FfiSublayerPlacement,
        device_pixel_ratio: f64,
    ) -> bool {
        // SAFETY: The host outlives this.
        unsafe { (self.0.remote_layer_can_show)(self.host(), context_id.0, kind, id, placement, device_pixel_ratio) }
    }

    pub fn remote_layer_can_show_high_dynamic_range(&self, context_id: CompositorContextId) -> bool {
        // SAFETY: The host outlives this.
        unsafe { (self.0.remote_layer_can_show_high_dynamic_range)(self.host(), context_id.0) }
    }

    pub fn build_sublayers(&self, context_id: CompositorContextId, sublayers: &[FfiSublayer]) -> SublayersHandle {
        // SAFETY: The host outlives this, and the presentable surfaces of the sublayers are live.
        let built = unsafe { (self.0.build_sublayers)(self.host(), context_id.0, ak::Span::new(sublayers)) };
        SublayersHandle {
            sublayers: NonNull::new(built).expect("the host builds a list of sublayers"),
            destroy: self.0.destroy_sublayers,
        }
    }

    pub fn frame_hand_off(&self, context_id: CompositorContextId) -> FrameHandOff {
        // SAFETY: The host outlives this.
        unsafe { (self.0.frame_hand_off)(self.host(), context_id.0) }
    }

    pub fn submit_frame(&self, context_id: CompositorContextId, surface: Option<&SurfaceHandle>) -> u64 {
        let surface = surface.map_or(std::ptr::null_mut(), SurfaceHandle::as_ptr);
        // SAFETY: The host outlives this, and the surface is live.
        unsafe { (self.0.submit_frame)(self.host(), context_id.0, surface) }
    }

    pub fn hand_off_once_gpu_work_is_scheduled(&self, token: u64) {
        // SAFETY: The host outlives this.
        unsafe { (self.0.hand_off_once_gpu_work_is_scheduled)(self.host(), token) };
    }

    /// Answers the presentation of the remote layer that showed the frame, or none where it could not show it.
    pub fn show_frame_in_remote_layer(
        &self,
        context_id: CompositorContextId,
        frame: &FfiRemoteLayerFrame,
        sublayers: SublayersHandle,
    ) -> Option<u64> {
        let mut presentation_id = 0;
        // SAFETY: The host outlives this, and takes the sublayers over.
        let shown = unsafe {
            (self.0.show_frame_in_remote_layer)(
                self.host(),
                context_id.0,
                frame,
                sublayers.into_raw(),
                &raw mut presentation_id,
            )
        };
        shown.then_some(presentation_id)
    }

    pub fn stop_showing_in_remote_layer(&self, context_id: CompositorContextId) {
        // SAFETY: The host outlives this.
        unsafe { (self.0.stop_showing_in_remote_layer)(self.host(), context_id.0) };
    }

    pub fn log_presented_frame(&self, page_id: u64, damage_rect: IntRect) {
        // SAFETY: The host outlives this.
        unsafe { (self.0.log_presented_frame)(self.host(), page_id, damage_rect) };
    }

    pub fn release_store_later(&self, context_id: CompositorContextId, bitmap_id: i32) {
        // SAFETY: The host outlives this.
        unsafe { (self.0.release_store_later)(self.host(), context_id.0, bitmap_id) };
    }

    pub fn request_vsync(&self, display_id: Option<u64>, refresh_rate: f64) {
        // SAFETY: The host outlives this.
        unsafe {
            (self.0.request_vsync)(
                self.host(),
                display_id.is_some(),
                display_id.unwrap_or_default(),
                refresh_rate,
            );
        }
    }

    pub fn most_recent_tick_time(
        &self,
        display_id: Option<u64>,
        now: MonotonicTime,
        refresh_rate: f64,
    ) -> Option<MonotonicTime> {
        let mut tick_time = 0;
        // SAFETY: The host outlives this.
        let has_tick = unsafe {
            (self.0.most_recent_tick_time)(
                self.host(),
                display_id.is_some(),
                display_id.unwrap_or_default(),
                now.nanoseconds(),
                refresh_rate,
                &raw mut tick_time,
            )
        };
        has_tick.then(|| MonotonicTime::from_nanoseconds(tick_time))
    }

    /// Answers the contexts that draw a video sink with a new frame.
    pub fn update_video_sinks(&self) -> Vec<CompositorContextId> {
        unsafe extern "C" fn receive(receiver: *mut c_void, context_ids: ak::Span<'_, u64>) {
            // SAFETY: The receiver is the list below, and the host lends the IDs for the call.
            let receiver = unsafe { &mut *receiver.cast::<Vec<CompositorContextId>>() };
            receiver.extend(context_ids.as_slice().iter().map(|id| CompositorContextId(*id)));
        }
        let mut context_ids = Vec::new();
        // SAFETY: The host outlives this, and answers synchronously.
        unsafe { (self.0.update_video_sinks)(self.host(), (&raw mut context_ids).cast(), receive) };
        context_ids
    }

    pub fn context_video_sinks_require_updates(&self, context_id: CompositorContextId) -> bool {
        // SAFETY: The host outlives this.
        unsafe { (self.0.context_video_sinks_require_updates)(self.host(), context_id.0) }
    }

    pub fn set_maximum_display_refresh_rate(&self, refresh_rate: f64) {
        // SAFETY: The host outlives this.
        unsafe { (self.0.set_maximum_display_refresh_rate)(self.host(), refresh_rate) };
    }

    /// Copies the tape and run table out of the shared memory they were sent in, and checks the copy.
    pub fn receive_display_list_storage(
        &self,
        buffer: Core_AnonymousBuffer,
        tape_size: u64,
        run_count: u64,
    ) -> Result<Arc<DisplayListStorage>, &'static str> {
        let (Ok(tape_size), Ok(run_count)) = (usize::try_from(tape_size), usize::try_from(run_count)) else {
            return Err("Display list sizes do not fit on this platform");
        };
        let Some((size, file)) = buffer.shared_memory else {
            // SAFETY: Nothing is read from an empty buffer.
            return unsafe { receive_packed_from_shared_memory(std::ptr::null(), 0, tape_size, run_count) };
        };
        #[cfg(unix)]
        let descriptor = std::os::fd::IntoRawFd::into_raw_fd(file.0);
        #[cfg(windows)]
        let descriptor = file.0.into_raw();
        let mut data = std::ptr::null();
        let mut data_size = 0;
        // SAFETY: The host outlives this, and takes the descriptor over.
        let mapping =
            unsafe { (self.0.map_shared_memory)(self.host(), descriptor, size, &raw mut data, &raw mut data_size) };
        if mapping.is_null() {
            return Err("Display list buffer is smaller than it claims");
        }
        // SAFETY: The mapping holds `data_size` readable bytes at `data` until it is dropped below.
        let storage = unsafe { receive_packed_from_shared_memory(data, data_size, tape_size, run_count) };
        // SAFETY: The mapping came from the host, which drops it once.
        unsafe { (self.0.unmap_shared_memory)(mapping) };
        storage
    }

    pub fn apply_resource_changes(&self, context_id: CompositorContextId, changes: &FfiResourceChanges<'_>) {
        // SAFETY: The host outlives this, and reads the changes during the call.
        unsafe { (self.0.apply_resource_changes)(self.host(), context_id.0, changes) };
    }

    pub fn staged_rasterization_has_resources(
        &self,
        client: u64,
        token: u64,
        font_ids: &[u64],
        image_frame_ids: &[u64],
    ) -> bool {
        // SAFETY: The host outlives this, and reads the IDs during the call.
        unsafe {
            (self.0.staged_rasterization_has_resources)(
                self.host(),
                client,
                token,
                ak::Span::new(font_ids),
                ak::Span::new(image_frame_ids),
            )
        }
    }

    /// Drops what the client staged under the token, without painting it.
    pub fn discard_staged_rasterization(&self, client: u64, token: u64) {
        // SAFETY: The host outlives this.
        unsafe { (self.0.finish_staged_rasterization)(self.host(), client, token) };
    }

    /// Paints into the target of a staged rasterization through a painter, which `paint` drives, and drops what was
    /// staged. Answers whether the target was painted.
    pub fn rasterize_staged(&self, client: u64, token: u64, paint: impl FnOnce(&mut FfiRasterPainter)) -> bool {
        let mut painter = std::mem::MaybeUninit::<FfiRasterPainter>::uninit();
        // SAFETY: The host outlives this, and writes the painter where it answers true.
        let began = unsafe { (self.0.begin_staged_rasterization)(self.host(), client, token, painter.as_mut_ptr()) };
        if began {
            // SAFETY: The host wrote the painter.
            paint(unsafe { painter.assume_init_mut() });
        }
        // SAFETY: The host outlives this.
        unsafe { (self.0.finish_staged_rasterization)(self.host(), client, token) };
        began
    }
}
