/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The mechanism that presenting a frame drives in the C++ around the core: Skia surfaces and the painter, the backing
//! stores of each context, the surfaces the window server shows, the media that live content comes from, GPU
//! submission, and the display ticks. None of these calls comes back into the core before it returns.

use crate::backing_stores::{FfiNewStore, NewStore, Publication, SharedImageToPublish, StoreKind};
use crate::cpp_object::{CppObject, FfiCppObject};
use crate::host::WebContentClientId;
use crate::remote_layers::{RemoteLayerHost, RemoteLayerObject, WindowServerObject};
use crate::surfaces::painting_surface::PaintingSurface;
use crate::surfaces::presentable_surface::PresentableSurface;
use crate::webgl::host::{
    ContextOptions, DefaultBuffers, ImageConversion, ImageConversionOutcome, PresentOutcome, VideoFrameCopy, WebGLHost,
};
use libcompositing_rust::display_list::commands::VideoSinkHandle;
use libcompositing_rust::display_list::commands::{CanvasId, CompositorContextId, DisplayListCommandType};
use libcompositing_rust::display_list::storage::{
    ListChunk, PackedChunkSize, receive_packed_chunks_from_shared_memory,
};
use libcompositing_rust::host::FfiRasterPainter;
use libcompositing_rust::ipc_types::{
    Compositing_RemoteLayerHostHandle, Compositing_RemoteLayerHostKind, Core_AnonymousBuffer, Gfx_AlphaType,
    Web_Bindings_PredefinedColorSpace,
};
use libcompositing_rust::time::MonotonicTime;
use libcompositing_rust::webgl::ImageSource;
use libgfx_rust::path::ffi::PathGeometryData;
use libgfx_rust::{Color, CornerRadii, FloatRect, IntRect, IntSize, ScalingMode, WindingRule};
use std::collections::HashMap;
use std::ffi::{CStr, c_char, c_void};
use std::ptr::NonNull;
use std::rc::Rc;

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

/// When a frame can be handed to whoever shows it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FrameHandOff {
    OnceGpuWorkIsScheduled,
    OnceGpuWorkIsFinished,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiCanvasContent {
    pub id: u64,
    pub content_generation: u64,
    /// Whether the window server can show the canvas as it is.
    pub has_presentable_surface: bool,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiVideoSinkContent {
    pub handle: u64,
    pub has_frame: bool,
    pub frame_id: u64,
    pub frame_holds_high_dynamic_range: bool,
    /// Whether the window server can show the frame as it is.
    pub frame_has_surface: bool,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiImageFrameContent {
    pub id: u64,
    pub pixel_generation: u64,
}

/// The live content of a context as it is now: the canvases asked about, every video sink the context draws and every
/// image frame its resources hold.
#[repr(C)]
pub struct FfiLiveContent<'a> {
    pub canvases: ak::Span<'a, FfiCanvasContent>,
    pub video_sinks: ak::Span<'a, FfiVideoSinkContent>,
    pub image_frames: ak::Span<'a, FfiImageFrameContent>,
}

/// The live content of a context as it is now: the canvases asked about by ID, every video sink the context draws by
/// its handle, and the pixel generation of every image frame by ID.
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
    /// Whether the content is also clipped by a path, which is in a space of its own.
    pub is_clipped_by_path: bool,
    pub clip_path_identity: u64,
    pub clip_path_fill_rule: WindingRule,
    /// The a, b, c, d, e and f of the affine transform from the space of the path to the frame.
    pub clip_path_to_frame: [f32; 6],
    /// Lent for the call that builds the sublayers, or that asks whether a remote layer can show the placement: the
    /// window server placement this is made from holds the path, and that of a sublayer lives in the layers of the
    /// frame painted last. A copy kept past that call, as the sublayers of the latest frame are, is never read
    /// through, so placements are told apart by the identity of the path, never by these contours.
    pub clip_path_contours: PathGeometryData,
    pub scaling_mode: ScalingMode,
}

impl PartialEq for FfiSublayerPlacement {
    fn eq(&self, other: &Self) -> bool {
        // Each field is named, so a new field does not compile until it is compared too.
        let Self {
            content_rect,
            content_to_frame,
            clip_rect,
            has_rounded_clip,
            rounded_clip_rect,
            rounded_clip_radius,
            rounds_top_left,
            rounds_top_right,
            rounds_bottom_right,
            rounds_bottom_left,
            is_clipped_by_path,
            clip_path_identity,
            clip_path_fill_rule,
            clip_path_to_frame,
            clip_path_contours: _,
            scaling_mode,
        } = self;
        *content_rect == other.content_rect
            && *content_to_frame == other.content_to_frame
            && *clip_rect == other.clip_rect
            && *has_rounded_clip == other.has_rounded_clip
            && *rounded_clip_rect == other.rounded_clip_rect
            && *rounded_clip_radius == other.rounded_clip_radius
            && *rounds_top_left == other.rounds_top_left
            && *rounds_top_right == other.rounds_top_right
            && *rounds_bottom_right == other.rounds_bottom_right
            && *rounds_bottom_left == other.rounds_bottom_left
            && *is_clipped_by_path == other.is_clipped_by_path
            && *clip_path_identity == other.clip_path_identity
            && *clip_path_fill_rule == other.clip_path_fill_rule
            && *clip_path_to_frame == other.clip_path_to_frame
            && *scaling_mode == other.scaling_mode
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum SublayerContentKind {
    Canvas,
    Video,
    PresentableSurface,
    /// Holds the sublayers that name it as their group, which the window server places and clips together.
    Group,
    /// The surface of a tile of a painted layer, shown at `rect` of the group it is in.
    Tile,
    /// A fill of `rect` of the group it is in with `color`.
    Color,
    /// What the window server shows below the sublayer, within a rect of the group it is in, run through filters.
    Backdrop,
}

/// What the window server shows below a sublayer, within `region` of the group the sublayer is in, run through a color
/// matrix, a blur and another color matrix, in that order, each of which can be left out. A matrix is row-major with
/// the offsets in the fifth column, for unpremultiplied colors from 0 to 1.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct FfiSublayerBackdrop {
    pub region: FloatRect,
    pub has_color_matrix_before_blur: bool,
    pub color_matrix_before_blur: [f32; 20],
    pub has_blur: bool,
    /// In device pixels.
    pub blur_standard_deviation: f32,
    pub has_color_matrix_after_blur: bool,
    pub color_matrix_after_blur: [f32; 20],
}

/// Where the window server places the sublayers of a group, how it clips them, and how it fades them together, in
/// device pixels.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct FfiSublayerGroup {
    /// The a, b, c, d, e and f of the affine transform from the pixels of the group to those of the group it is in, or
    /// to those of the frame.
    pub to_parent: [f32; 6],
    /// Whether the group clips its sublayers to `clip_rect`, in its own pixels, with the corners of the rect rounded
    /// by the radii.
    pub clips: bool,
    pub clip_rect: FloatRect,
    pub corner_radii: CornerRadii,
    /// The opacity the group composites its sublayers at, once they are composited with each other.
    pub opacity: f32,
}

impl Default for FfiSublayerGroup {
    fn default() -> Self {
        Self {
            to_parent: [1.0, 0.0, 0.0, 1.0, 0.0, 0.0],
            clips: false,
            clip_rect: FloatRect::default(),
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        }
    }
}

/// A sublayer the window server shows of a frame: a canvas of the context by its id, a video by the handle of its sink,
/// a surface, a tile, a fill, or a group of other sublayers.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct FfiSublayer {
    pub identity: u64,
    /// The index, among the sublayers of the frame, of the group the sublayer is in, or -1 for one in the frame itself.
    /// A group comes before the sublayers in it, and sublayers of one group are shown bottom to top.
    pub group_index: i32,
    pub content_kind: SublayerContentKind,
    pub media_id: u64,
    /// Of a surface or a tile.
    pub presentable_surface: *const PresentableSurface,
    /// Of a canvas, a video or a surface.
    pub placement: FfiSublayerPlacement,
    pub group: FfiSublayerGroup,
    /// Of a tile or a fill, in the pixels of the group it is in.
    pub rect: IntRect,
    pub color: Color,
    /// Of a tile.
    pub every_pixel_is_opaque: bool,
    pub backdrop: FfiSublayerBackdrop,
}

impl FfiSublayer {
    /// A sublayer of the frame itself that shows nothing, for the fields of each kind of content to be set on.
    pub fn of_kind(identity: u64, content_kind: SublayerContentKind) -> Self {
        Self {
            identity,
            group_index: -1,
            content_kind,
            media_id: 0,
            presentable_surface: std::ptr::null(),
            placement: FfiSublayerPlacement::default(),
            group: FfiSublayerGroup::default(),
            rect: IntRect::default(),
            color: Color::TRANSPARENT,
            every_pixel_is_opaque: false,
            backdrop: FfiSublayerBackdrop::default(),
        }
    }
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiRemoteLayerFrame {
    /// Lent for the call. Without the store, the store shown before stays, and only the sublayers change, unless the
    /// frame shows no store.
    pub store: *const PresentableSurface,
    /// Whether the sublayers are all of the frame, which then shows no store below them.
    pub shows_no_store: bool,
    pub painted_size: IntSize,
    pub device_pixel_ratio: f64,
    /// Where the frame is scaled to fit within the view, in device pixels, rather than shown at its own size.
    pub has_fit_within: bool,
    pub fit_within: IntSize,
}

/// How the presentation shows a remote layer: the kind of its host handle, which is that of
/// `Compositing::RemoteLayerHostKind`, and its id.
#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiRemoteLayerHostHandle {
    pub kind: u8,
    pub id: u64,
}

/// What the window server that shows a context composites by itself of the layers of its frames, besides the canvases
/// and videos it shows.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiWindowServerCompositing {
    /// Whether it shows each tile of a painted layer as a sublayer of its own, in a group it places and clips.
    pub shows_tiles_of_painted_layers: bool,
    /// Whether it runs what it shows below a sublayer through a blur and color matrices.
    pub filters_what_is_below_layers: bool,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiCanvas2DPainter {
    pub player: *mut c_void,
    /// Plays a command, whose payload is lent for the call.
    pub play_command: unsafe extern "C" fn(*mut c_void, DisplayListCommandType, ak::Span<'_, u8>),
}

/// The entry points of the mechanism the core drives, and the object they are called on:
/// `Compositor::RustFFI::FfiRasterHost`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiRasterHost {
    pub host: *mut c_void,

    /// A new surface with one reference that the caller takes over, or null where it could not be made. The surface is
    /// made like the one given, or for the GPU the compositor renders with where none is given. The last argument also
    /// inherits LCD text for pixel-aligned copies.
    pub create_surface:
        unsafe extern "C" fn(*mut c_void, IntSize, *const PaintingSurface, bool) -> *const PaintingSurface,
    pub surface_save: unsafe extern "C" fn(*const PaintingSurface) -> i32,
    pub surface_restore_to_count: unsafe extern "C" fn(*const PaintingSurface, i32),
    pub surface_translate: unsafe extern "C" fn(*const PaintingSurface, f32, f32),
    pub surface_scale: unsafe extern "C" fn(*const PaintingSurface, f32, f32),
    pub surface_clip_rect: unsafe extern "C" fn(*const PaintingSurface, IntRect),
    pub surface_clear: unsafe extern "C" fn(*const PaintingSurface, Color),
    /// Arguments: the target, the source, the rect of the source drawn, the rect of the target it is drawn into, and
    /// how it is blended.
    pub surface_draw_surface:
        unsafe extern "C" fn(*const PaintingSurface, *const PaintingSurface, IntRect, IntRect, SurfaceBlend),
    pub flush_surface: unsafe extern "C" fn(*const PaintingSurface),

    /// Arguments: the host, the context, the surface, whose resources it draws with, whether it allows LCD text,
    /// and where the painter is written. The painting lasts until `end_painting`, and paintings nest.
    pub begin_painting:
        unsafe extern "C" fn(*mut c_void, u64, *const PaintingSurface, PaintingResources, bool, *mut FfiRasterPainter),
    pub end_painting: unsafe extern "C" fn(*mut c_void),
    /// Lets the raster caches of the context's page drop what the frames before the one that starts did not use.
    pub start_raster_cache_frame: unsafe extern "C" fn(*mut c_void, u64),

    /// Makes a store of the context, writing what it made. Answers false where none could be made.
    /// Arguments: the context, the kind and size of the store, the window server that shows it, if one does, and where
    /// the store is written.
    pub create_store: unsafe extern "C" fn(*mut c_void, u64, StoreKind, IntSize, *mut c_void, *mut FfiNewStore) -> bool,
    /// Arguments: the presentation, the context, whether the stores were added to those it has rather than replacing
    /// them, and the stores with their images, which it takes over.
    pub publish_stores: unsafe extern "C" fn(*mut c_void, u64, u64, bool, ak::Span<'_, i32>, ak::Span<'_, *mut c_void>),
    /// Arguments: the presentation, the context, and the stores that it no longer has.
    pub retire_published_stores: unsafe extern "C" fn(*mut c_void, u64, u64, ak::Span<'_, i32>),

    /// A new surface of the size, made like the one given, with one reference that the caller takes over, or null
    /// where none could be made.
    pub create_presentable_surface:
        unsafe extern "C" fn(*mut c_void, IntSize, *const PaintingSurface) -> *const PresentableSurface,
    /// What the window server composites by itself of the layers of the frames its remote layers show.
    pub window_server_compositing: unsafe extern "C" fn(*mut c_void) -> FfiWindowServerCompositing,

    pub live_content: unsafe extern "C" fn(
        *mut c_void,
        u64,
        ak::Span<'_, u64>,
        *mut c_void,
        unsafe extern "C" fn(*mut c_void, *const FfiLiveContent<'_>),
    ),
    /// Arguments: the remote layer, the context, the media, where it is placed, and the device pixel ratio.
    pub remote_layer_can_show:
        unsafe extern "C" fn(*mut c_void, *mut c_void, u64, MediaKind, u64, *const FfiSublayerPlacement, f64) -> bool,
    /// Resolves the content of the sublayers, which the window server then holds busy until the list is destroyed or
    /// shown.
    pub build_sublayers: unsafe extern "C" fn(*mut c_void, u64, ak::Span<'_, FfiSublayer>) -> *mut c_void,
    pub destroy_sublayers: unsafe extern "C" fn(*mut c_void),

    /// Submits the GPU work drawn into the surface, or into the surfaces the window server shows where it is null,
    /// answering a token that `compositor_core_gpu_work_finished` hands back once it is done, on a later turn.
    pub submit_frame: unsafe extern "C" fn(*mut c_void, u64, *const PaintingSurface) -> u64,
    /// Calls `compositor_core_frame_ready_for_hand_off` with the token on a later turn, once the work is scheduled.
    pub hand_off_once_gpu_work_is_scheduled: unsafe extern "C" fn(*mut c_void, u64),
    /// Writes the window server of a new presentation, or nothing where frames reach the screen only through the
    /// presentation.
    pub create_window_server: unsafe extern "C" fn(*mut c_void, *mut FfiCppObject) -> bool,
    pub window_server_frame_hand_off: unsafe extern "C" fn(*mut c_void) -> FrameHandOff,
    /// While held, what every remote layer of the window server shows changes nowhere on screen, and changes all at
    /// once when released.
    pub window_server_hold_changes_on_screen: unsafe extern "C" fn(*mut c_void),
    pub window_server_release_changes_held_off_screen: unsafe extern "C" fn(*mut c_void),
    /// Arguments: the window server, the id of the layer, and where the layer and its host handle are written.
    pub create_remote_layer:
        unsafe extern "C" fn(*mut c_void, u64, *mut FfiCppObject, *mut FfiRemoteLayerHostHandle) -> bool,
    /// Arguments: the layer, whether the frame is scaled to fit, and the size it fits within.
    pub remote_layer_place_frame: unsafe extern "C" fn(*mut c_void, bool, IntSize),
    /// Shows the frame in the layer with the sublayers given, which it takes over. Answers false where the layer could
    /// not show it.
    pub remote_layer_show_frame: unsafe extern "C" fn(*mut c_void, *const FfiRemoteLayerFrame, *mut c_void) -> bool,
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
    pub map_shared_memory: unsafe extern "C" fn(*mut c_void, i32, u64, *mut *mut u8, *mut usize) -> *mut c_void,
    pub unmap_shared_memory: unsafe extern "C" fn(*mut c_void),
    /// Arguments: the context, and the ids of the image frames that a display list or a visual context tree draws.
    /// Has the context hold those it lacks of the image frames its client registered, and answers whether it then
    /// holds all of them.
    pub take_registered_image_frames: unsafe extern "C" fn(*mut c_void, u64, ak::Span<'_, u64>) -> bool,
    /// Arguments: the context, and the ids of the image frames that its display list and its visual context tree
    /// draw, which are the image frames the host keeps for it.
    pub set_drawn_image_frames: unsafe extern "C" fn(*mut c_void, u64, ak::Span<'_, u64>),
    /// Arguments: the context, and the handles of the video sinks that its display list draws.
    pub set_drawn_video_sinks: unsafe extern "C" fn(*mut c_void, u64, ak::Span<'_, u64>),
    /// Arguments: the context, and the ids of the SVG images that its display list draws, whose rasters the host keeps.
    pub set_drawn_vector_images: unsafe extern "C" fn(*mut c_void, u64, ak::Span<'_, u64>),
    /// Arguments: the context, and the ids of the fonts that its display list draws, which are the fonts the host
    /// keeps for it.
    pub set_drawn_fonts: unsafe extern "C" fn(*mut c_void, u64, ak::Span<'_, u64>),
    /// Arguments: the context, and the ids of the fonts that a display list draws. Answers whether the host holds all
    /// of them for the context.
    pub context_holds_fonts: unsafe extern "C" fn(*mut c_void, u64, ak::Span<'_, u64>) -> bool,
    /// Arguments: the client, the token it staged the rasterization under, and the fonts and image frames the display
    /// list draws. Answers whether the rasterization is staged with all of them. A staged rasterization draws no video
    /// frames, so the sinks are not asked about.
    pub staged_rasterization_has_resources:
        unsafe extern "C" fn(*mut c_void, u64, u64, ak::Span<'_, u64>, ak::Span<'_, u64>) -> bool,
    /// Writes the painter that paints into the target of the staged rasterization, with its resources, until
    /// `finish_staged_rasterization`. Answers false for a target that cannot be painted into.
    pub begin_staged_rasterization: unsafe extern "C" fn(*mut c_void, u64, u64, *mut FfiRasterPainter) -> bool,
    /// Ends the painting that began, flushes it into the target, and drops what was staged.
    pub finish_staged_rasterization: unsafe extern "C" fn(*mut c_void, u64, u64),

    /// Reserves canvas IDs that name nothing else, as many as asked for, and answers the first of them.
    pub reserve_canvas_ids: unsafe extern "C" fn(*mut c_void, u64) -> u64,
    /// Arguments: the client, the canvas, the size of the context, whether it has alpha, whether it will be read
    /// frequently, and the color space of its pixels. Answers whether the context could be made.
    pub create_canvas_2d_context: unsafe extern "C" fn(
        *mut c_void,
        u64,
        CanvasId,
        IntSize,
        bool,
        bool,
        Web_Bindings_PredefinedColorSpace,
    ) -> bool,
    pub destroy_canvas_2d_context: unsafe extern "C" fn(*mut c_void, u64, CanvasId),
    /// Arguments: the client, the canvas, the rect, the alpha type and the color space of the pixels, where they go,
    /// how many bytes fit there, and where the size of what was read is written.
    pub read_back_canvas_2d_pixels: unsafe extern "C" fn(
        *mut c_void,
        u64,
        CanvasId,
        IntRect,
        u8,
        Web_Bindings_PredefinedColorSpace,
        *mut u8,
        usize,
        *mut IntSize,
    ) -> bool,
    /// Arguments: the client, the canvas of a context it made, and where the painter is written. The painting lasts
    /// until `end_canvas_2d_painting`.
    pub begin_canvas_2d_painting: unsafe extern "C" fn(*mut c_void, u64, CanvasId, *mut FfiCanvas2DPainter),
    /// Answers whether the painting sampled a frame of a video on the GPU.
    pub end_canvas_2d_painting: unsafe extern "C" fn(*mut c_void) -> bool,
    /// Arguments: the client and the canvas. Makes what the context drew the content the canvas shows, and answers
    /// whether it could.
    pub present_canvas_2d_context: unsafe extern "C" fn(*mut c_void, u64, CanvasId) -> bool,
    /// Arguments: the client and canvases of its contexts. Submits the GPU work drawn into their surfaces.
    pub flush_canvas_2d_contexts: unsafe extern "C" fn(*mut c_void, u64, ak::Span<'_, CanvasId>),

    /// Answers the address of the GL function of the name, or null.
    pub gl_proc_address: unsafe extern "C" fn(*mut c_void, *const c_char) -> *const c_void,
    /// Arguments: the client, the canvas, and what its context is made with. Answers whether a GL context with a
    /// drawing buffer could be made.
    pub create_webgl_context: unsafe extern "C" fn(*mut c_void, u64, CanvasId, ContextOptions) -> bool,
    pub destroy_webgl_context: unsafe extern "C" fn(*mut c_void, u64, CanvasId),
    /// Makes the GL context of the client's canvas current, and writes its default framebuffer and renderbuffer.
    pub make_webgl_context_current: unsafe extern "C" fn(*mut c_void, u64, CanvasId, *mut DefaultBuffers),
    pub set_webgl_drawing_buffer_size: unsafe extern "C" fn(*mut c_void, u64, CanvasId, IntSize),
    /// Arguments: the client, the canvas, whether the drawing buffer is preserved, and whether the frame presented last
    /// goes on being shown where there is one.
    pub present_webgl_context: unsafe extern "C" fn(*mut c_void, u64, CanvasId, bool, bool) -> PresentOutcome,
    /// Arguments: the client, the canvas, whether the frame presented last is read rather than the drawing buffer, the
    /// rect, where the pixels go, how many bytes fit there, and where the size of what was read is written.
    pub read_back_webgl_drawing_buffer:
        unsafe extern "C" fn(*mut c_void, u64, CanvasId, bool, IntRect, *mut u8, usize, *mut IntSize) -> bool,
    pub webgl_can_copy_images_into_textures: unsafe extern "C" fn(*mut c_void, u64, CanvasId) -> bool,
    /// Arguments: the client, the sink of the video, the generation of its frame, and where the frame and its size are
    /// written. Answers whether the client holds that frame.
    pub held_video_frame: unsafe extern "C" fn(*mut c_void, u64, u64, u64, *mut FfiCppObject, *mut IntSize) -> bool,
    /// Writes an image of the frame on the GPU, which is null where there cannot be one.
    pub video_frame_image: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut FfiCppObject),
    pub copy_video_frame_image_into_bound_texture:
        unsafe extern "C" fn(*mut c_void, u64, CanvasId, *mut c_void, VideoFrameCopy) -> bool,
    pub convert_image_source_for_webgl: unsafe extern "C" fn(
        *mut c_void,
        u64,
        FfiImageSource,
        FfiImageConversion,
        *mut FfiConvertedPixels,
    ) -> FfiImageConversionOutcome,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiImageSource {
    pub is_held_video_frame: bool,
    pub id: u64,
    pub generation: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiImageConversion {
    /// A `Gfx::ExportFormat`.
    pub export_format: u8,
    pub flip_y: bool,
    pub premultiply_alpha: bool,
    pub has_target_size: bool,
    pub target_size: IntSize,
}

#[repr(C)]
pub struct FfiConvertedPixels {
    pub pixels: FfiCppObject,
    pub data: *const u8,
    pub size: usize,
    pub width: i32,
    pub height: i32,
}

#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum FfiImageConversionOutcome {
    Converted,
    UnknownImageFrame,
    Failed,
}

pub struct ConvertedPixels {
    _owner: CppObject,
    data: *const u8,
    size: usize,
}

impl AsRef<[u8]> for ConvertedPixels {
    fn as_ref(&self) -> &[u8] {
        // SAFETY: The owner keeps the bytes for as long as this lives, and nothing writes them.
        unsafe { ak::slice_from_raw_parts(self.data, self.size) }
    }
}

/// Shared memory that WebContent sent, mapped until this is dropped.
pub struct SharedMemory {
    mapping: NonNull<c_void>,
    data: *mut u8,
    len: usize,
    unmap: unsafe extern "C" fn(*mut c_void),
}

impl SharedMemory {
    pub fn as_ptr(&self) -> *mut u8 {
        self.data
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }
}

impl Drop for SharedMemory {
    fn drop(&mut self) {
        // SAFETY: The mapping came from the host, and nothing uses it after this.
        unsafe { (self.unmap)(self.mapping.as_ptr()) };
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

    pub fn create_surface(
        &self,
        size: IntSize,
        like: Option<&Rc<PaintingSurface>>,
        inherits_lcd_text: bool,
    ) -> Option<Rc<PaintingSurface>> {
        let like = like.map_or(std::ptr::null(), Rc::as_ptr);
        // SAFETY: The host outlives this, and hands over a new surface or null.
        unsafe {
            PaintingSurface::adopted_from_ffi((self.0.create_surface)(self.host(), size, like, inherits_lcd_text))
        }
    }

    pub fn start_raster_cache_frame(&self, context_id: CompositorContextId) {
        // SAFETY: The host outlives this.
        unsafe { (self.0.start_raster_cache_frame)(self.host(), context_id.0) };
    }

    pub fn save(&self, surface: &Rc<PaintingSurface>) -> i32 {
        // SAFETY: The surface is live.
        unsafe { (self.0.surface_save)(Rc::as_ptr(surface)) }
    }

    pub fn restore_to_count(&self, surface: &Rc<PaintingSurface>, count: i32) {
        // SAFETY: The surface is live.
        unsafe { (self.0.surface_restore_to_count)(Rc::as_ptr(surface), count) };
    }

    pub fn translate(&self, surface: &Rc<PaintingSurface>, x: f32, y: f32) {
        // SAFETY: The surface is live.
        unsafe { (self.0.surface_translate)(Rc::as_ptr(surface), x, y) };
    }

    pub fn scale(&self, surface: &Rc<PaintingSurface>, x: f32, y: f32) {
        // SAFETY: The surface is live.
        unsafe { (self.0.surface_scale)(Rc::as_ptr(surface), x, y) };
    }

    pub fn clip_rect(&self, surface: &Rc<PaintingSurface>, rect: IntRect) {
        // SAFETY: The surface is live.
        unsafe { (self.0.surface_clip_rect)(Rc::as_ptr(surface), rect) };
    }

    pub fn clear(&self, surface: &Rc<PaintingSurface>, color: Color) {
        // SAFETY: The surface is live.
        unsafe { (self.0.surface_clear)(Rc::as_ptr(surface), color) };
    }

    pub fn draw_surface(
        &self,
        target: &Rc<PaintingSurface>,
        source: &Rc<PaintingSurface>,
        source_rect: IntRect,
        target_rect: IntRect,
        blend: SurfaceBlend,
    ) {
        // SAFETY: Both surfaces are live.
        unsafe {
            (self.0.surface_draw_surface)(Rc::as_ptr(target), Rc::as_ptr(source), source_rect, target_rect, blend);
        }
    }

    pub fn flush(&self, surface: &Rc<PaintingSurface>) {
        // SAFETY: The surface is live.
        unsafe { (self.0.flush_surface)(Rc::as_ptr(surface)) };
    }

    /// Paints into the surface through a painter, which `paint` drives.
    pub fn paint(
        &self,
        context_id: CompositorContextId,
        surface: &Rc<PaintingSurface>,
        resources: PaintingResources,
        allows_lcd_text: bool,
        paint: impl FnOnce(&mut FfiRasterPainter),
    ) {
        let mut painter = std::mem::MaybeUninit::<FfiRasterPainter>::uninit();
        // SAFETY: The host outlives this, the surface is live, and the host writes the painter.
        let mut painter = unsafe {
            (self.0.begin_painting)(
                self.host(),
                context_id.0,
                Rc::as_ptr(surface),
                resources,
                allows_lcd_text,
                painter.as_mut_ptr(),
            );
            painter.assume_init()
        };
        paint(&mut painter);
        // SAFETY: The host outlives this, and the painting began above.
        unsafe { (self.0.end_painting)(self.host()) };
    }

    pub fn create_store(
        &self,
        context_id: CompositorContextId,
        kind: StoreKind,
        size: IntSize,
        window_server: Option<&WindowServerObject>,
    ) -> Option<NewStore> {
        let mut store = FfiNewStore::default();
        let window_server = window_server.map_or(std::ptr::null_mut(), WindowServerObject::as_ptr);
        // SAFETY: The host outlives this, the window server is live, and the host writes the store it made.
        if !unsafe { (self.0.create_store)(self.host(), context_id.0, kind, size, window_server, &raw mut store) } {
            return None;
        }
        // SAFETY: The host handed the store over.
        unsafe { NewStore::adopt(store) }
    }

    pub fn publish_stores(
        &self,
        presentation_id: u64,
        context_id: CompositorContextId,
        added: bool,
        publication: Publication,
    ) {
        let shared_images: Vec<*mut c_void> = publication
            .shared_images
            .into_iter()
            .map(SharedImageToPublish::into_raw)
            .collect();
        // SAFETY: The host outlives this, and takes the images over.
        unsafe {
            (self.0.publish_stores)(
                self.host(),
                presentation_id,
                context_id.0,
                added,
                ak::Span::new(&publication.bitmap_ids),
                ak::Span::new(&shared_images),
            );
        }
    }

    pub fn retire_published_stores(&self, presentation_id: u64, context_id: CompositorContextId, bitmap_ids: &[i32]) {
        // SAFETY: The host outlives this.
        unsafe {
            (self.0.retire_published_stores)(self.host(), presentation_id, context_id.0, ak::Span::new(bitmap_ids));
        }
    }

    pub fn create_presentable_surface(
        &self,
        size: IntSize,
        like: &Rc<PaintingSurface>,
    ) -> Option<Rc<PresentableSurface>> {
        // SAFETY: The host outlives this, and hands over a new surface or null.
        unsafe {
            PresentableSurface::adopted_from_ffi((self.0.create_presentable_surface)(
                self.host(),
                size,
                Rc::as_ptr(like),
            ))
        }
    }

    pub fn window_server_compositing(&self, window_server: &WindowServerObject) -> FfiWindowServerCompositing {
        // SAFETY: The window server is live.
        unsafe { (self.0.window_server_compositing)(window_server.as_ptr()) }
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
                .map(|sink| (sink.handle, *sink))
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
        remote_layer: &RemoteLayerObject,
        context_id: CompositorContextId,
        kind: MediaKind,
        id: u64,
        placement: &FfiSublayerPlacement,
        device_pixel_ratio: f64,
    ) -> bool {
        // SAFETY: The host outlives this, and the layer is live.
        unsafe {
            (self.0.remote_layer_can_show)(
                self.host(),
                remote_layer.as_ptr(),
                context_id.0,
                kind,
                id,
                placement,
                device_pixel_ratio,
            )
        }
    }

    pub fn build_sublayers(&self, context_id: CompositorContextId, sublayers: &[FfiSublayer]) -> SublayersHandle {
        // SAFETY: The host outlives this, and the presentable surfaces of the sublayers are live.
        let built = unsafe { (self.0.build_sublayers)(self.host(), context_id.0, ak::Span::new(sublayers)) };
        SublayersHandle {
            sublayers: NonNull::new(built).expect("the host builds a list of sublayers"),
            destroy: self.0.destroy_sublayers,
        }
    }

    /// When a frame shown through the presentation can be handed to it.
    pub fn frame_hand_off_through_presentation() -> FrameHandOff {
        if cfg!(target_os = "macos") {
            FrameHandOff::OnceGpuWorkIsScheduled
        } else {
            FrameHandOff::OnceGpuWorkIsFinished
        }
    }

    pub fn window_server_frame_hand_off(&self, window_server: &WindowServerObject) -> FrameHandOff {
        // SAFETY: The window server is live.
        unsafe { (self.0.window_server_frame_hand_off)(window_server.as_ptr()) }
    }

    pub fn submit_frame(&self, context_id: CompositorContextId, surface: Option<&Rc<PaintingSurface>>) -> u64 {
        let surface = surface.map_or(std::ptr::null(), Rc::as_ptr);
        // SAFETY: The host outlives this, and the surface is live.
        unsafe { (self.0.submit_frame)(self.host(), context_id.0, surface) }
    }

    pub fn hand_off_once_gpu_work_is_scheduled(&self, token: u64) {
        // SAFETY: The host outlives this.
        unsafe { (self.0.hand_off_once_gpu_work_is_scheduled)(self.host(), token) };
    }

    pub fn remote_layer_show_frame(
        &self,
        remote_layer: &RemoteLayerObject,
        frame: &FfiRemoteLayerFrame,
        sublayers: SublayersHandle,
    ) -> bool {
        // SAFETY: The layer is live, and takes the sublayers over.
        unsafe { (self.0.remote_layer_show_frame)(remote_layer.as_ptr(), frame, sublayers.into_raw()) }
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

    /// Copies the chunks packed in the shared memory they were sent in, whose sizes `sizes` lists, out, checks each copy
    /// and names it by what it holds.
    pub fn receive_display_list_chunks(
        &self,
        buffer: Core_AnonymousBuffer,
        sizes: &[PackedChunkSize],
    ) -> Result<Vec<ListChunk>, &'static str> {
        let Some(memory) = self.map_shared_memory(buffer) else {
            return Err("Display list buffer is smaller than it claims");
        };
        let Some(memory) = memory else {
            // SAFETY: Nothing is read from an empty buffer.
            return unsafe { receive_packed_chunks_from_shared_memory(std::ptr::null(), 0, sizes) };
        };
        // SAFETY: The mapping holds as many readable bytes until it is dropped, after the chunks are copied out.
        unsafe { receive_packed_chunks_from_shared_memory(memory.as_ptr(), memory.len(), sizes) }
    }

    /// Maps shared memory that WebContent sent, which is empty where the buffer holds nothing. None where the buffer
    /// does not hold as much memory as it claims.
    pub fn map_shared_memory(&self, buffer: Core_AnonymousBuffer) -> Option<Option<SharedMemory>> {
        let Some((size, file)) = buffer.shared_memory else {
            return Some(None);
        };
        #[cfg(unix)]
        let descriptor = std::os::fd::IntoRawFd::into_raw_fd(file.0);
        #[cfg(windows)]
        let descriptor = file.0.into_raw();
        let mut data = std::ptr::null_mut();
        let mut mapped_size = 0;
        // SAFETY: The host outlives this, and takes the descriptor over.
        let mapping =
            unsafe { (self.0.map_shared_memory)(self.host(), descriptor, size, &raw mut data, &raw mut mapped_size) };
        let mut memory = SharedMemory {
            mapping: NonNull::new(mapping)?,
            data,
            len: mapped_size,
            unmap: self.0.unmap_shared_memory,
        };
        memory.len = usize::try_from(size).ok().filter(|&size| size <= mapped_size)?;
        Some(Some(memory))
    }

    pub fn take_registered_image_frames(&self, context_id: CompositorContextId, image_frame_ids: &[u64]) -> bool {
        // SAFETY: The host outlives this, and reads the ids during the call.
        unsafe { (self.0.take_registered_image_frames)(self.host(), context_id.0, ak::Span::new(image_frame_ids)) }
    }

    pub fn set_drawn_image_frames(&self, context_id: CompositorContextId, image_frame_ids: &[u64]) {
        // SAFETY: The host outlives this, and reads the ids during the call.
        unsafe { (self.0.set_drawn_image_frames)(self.host(), context_id.0, ak::Span::new(image_frame_ids)) };
    }

    pub fn set_drawn_video_sinks(&self, context_id: CompositorContextId, video_sink_handles: &[u64]) {
        // SAFETY: The host outlives this, and reads the handles during the call.
        unsafe { (self.0.set_drawn_video_sinks)(self.host(), context_id.0, ak::Span::new(video_sink_handles)) };
    }

    pub fn set_drawn_vector_images(&self, context_id: CompositorContextId, vector_image_ids: &[u64]) {
        // SAFETY: The host outlives this, and reads the ids during the call.
        unsafe { (self.0.set_drawn_vector_images)(self.host(), context_id.0, ak::Span::new(vector_image_ids)) };
    }

    pub fn set_drawn_fonts(&self, context_id: CompositorContextId, font_ids: &[u64]) {
        // SAFETY: The host outlives this, and reads the ids during the call.
        unsafe { (self.0.set_drawn_fonts)(self.host(), context_id.0, ak::Span::new(font_ids)) };
    }

    pub fn context_holds_fonts(&self, context_id: CompositorContextId, font_ids: &[u64]) -> bool {
        // SAFETY: The host outlives this, and reads the ids during the call.
        unsafe { (self.0.context_holds_fonts)(self.host(), context_id.0, ak::Span::new(font_ids)) }
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

    pub fn reserve_canvas_ids(&self, count: u64) -> u64 {
        // SAFETY: The host outlives this.
        unsafe { (self.0.reserve_canvas_ids)(self.host(), count) }
    }

    pub fn create_canvas_2d_context(
        &self,
        client: u64,
        canvas_id: CanvasId,
        size: IntSize,
        alpha: bool,
        will_read_frequently: bool,
        color_space: Web_Bindings_PredefinedColorSpace,
    ) -> bool {
        // SAFETY: The host outlives this.
        unsafe {
            (self.0.create_canvas_2d_context)(
                self.host(),
                client,
                canvas_id,
                size,
                alpha,
                will_read_frequently,
                color_space,
            )
        }
    }

    pub fn destroy_canvas_2d_context(&self, client: u64, canvas_id: CanvasId) {
        // SAFETY: The host outlives this.
        unsafe { (self.0.destroy_canvas_2d_context)(self.host(), client, canvas_id) };
    }

    pub fn read_back_canvas_2d_pixels(
        &self,
        client: u64,
        canvas_id: CanvasId,
        rect: IntRect,
        alpha_type: Gfx_AlphaType,
        color_space: Web_Bindings_PredefinedColorSpace,
        pixels: &SharedMemory,
    ) -> Option<IntSize> {
        let mut size = IntSize::default();
        // SAFETY: The host outlives this, the mapping holds as many writable bytes, and the host writes the size.
        unsafe {
            (self.0.read_back_canvas_2d_pixels)(
                self.host(),
                client,
                canvas_id,
                rect,
                alpha_type as u8,
                color_space,
                pixels.as_ptr(),
                pixels.len(),
                &raw mut size,
            )
        }
        .then_some(size)
    }

    pub fn begin_canvas_2d_painting(&self, client: u64, canvas_id: CanvasId) -> Canvas2DPainting<'_> {
        let mut painter = std::mem::MaybeUninit::<FfiCanvas2DPainter>::uninit();
        // SAFETY: The host outlives this, and writes the painter.
        unsafe { (self.0.begin_canvas_2d_painting)(self.host(), client, canvas_id, painter.as_mut_ptr()) };
        Canvas2DPainting {
            raster: self,
            // SAFETY: The host wrote the painter.
            painter: unsafe { painter.assume_init() },
        }
    }

    pub fn try_present_canvas_2d_context(&self, client: u64, canvas_id: CanvasId) -> bool {
        // SAFETY: The host outlives this.
        unsafe { (self.0.present_canvas_2d_context)(self.host(), client, canvas_id) }
    }

    pub fn flush_canvas_2d_contexts(&self, client: u64, canvas_ids: &[CanvasId]) {
        // SAFETY: The host outlives this, and reads the IDs during the call.
        unsafe { (self.0.flush_canvas_2d_contexts)(self.host(), client, ak::Span::new(canvas_ids)) };
    }
}

impl WebGLHost for RasterHost {
    type Pixels = ConvertedPixels;
    type VideoFrame = CppObject;
    type VideoFrameImage = CppObject;

    fn gl_proc_address(&self, name: &CStr) -> *const c_void {
        // SAFETY: The host outlives this, and reads the name during the call.
        unsafe { (self.0.gl_proc_address)(self.host(), name.as_ptr()) }
    }

    fn allocate_canvas_id(&self) -> CanvasId {
        CanvasId(self.reserve_canvas_ids(1))
    }

    fn create_webgl_context(&self, client: WebContentClientId, canvas_id: CanvasId, options: ContextOptions) -> bool {
        // SAFETY: The host outlives this.
        unsafe { (self.0.create_webgl_context)(self.host(), client.0, canvas_id, options) }
    }

    fn destroy_webgl_context(&self, client: WebContentClientId, canvas_id: CanvasId) {
        // SAFETY: The host outlives this.
        unsafe { (self.0.destroy_webgl_context)(self.host(), client.0, canvas_id) };
    }

    fn make_webgl_context_current(&self, client: WebContentClientId, canvas_id: CanvasId) -> DefaultBuffers {
        let mut defaults = DefaultBuffers::default();
        // SAFETY: The host outlives this, and writes the default buffers.
        unsafe { (self.0.make_webgl_context_current)(self.host(), client.0, canvas_id, &raw mut defaults) };
        defaults
    }

    fn set_webgl_drawing_buffer_size(&self, client: WebContentClientId, canvas_id: CanvasId, size: IntSize) {
        // SAFETY: The host outlives this.
        unsafe { (self.0.set_webgl_drawing_buffer_size)(self.host(), client.0, canvas_id, size) };
    }

    fn present_webgl_context(
        &self,
        client: WebContentClientId,
        canvas_id: CanvasId,
        preserve_drawing_buffer: bool,
        show_presented_frame: bool,
    ) -> PresentOutcome {
        // SAFETY: The host outlives this.
        unsafe {
            (self.0.present_webgl_context)(
                self.host(),
                client.0,
                canvas_id,
                preserve_drawing_buffer,
                show_presented_frame,
            )
        }
    }

    unsafe fn read_back_webgl_drawing_buffer(
        &self,
        client: WebContentClientId,
        canvas_id: CanvasId,
        from_presented_frame: bool,
        rect: IntRect,
        pixels: *mut u8,
        pixels_size: usize,
    ) -> Option<IntSize> {
        let mut size = IntSize::default();
        // SAFETY: The host outlives this, the caller passes as many writable bytes, and the host writes the size.
        unsafe {
            (self.0.read_back_webgl_drawing_buffer)(
                self.host(),
                client.0,
                canvas_id,
                from_presented_frame,
                rect,
                pixels,
                pixels_size,
                &raw mut size,
            )
        }
        .then_some(size)
    }

    fn webgl_can_copy_images_into_textures(&self, client: WebContentClientId, canvas_id: CanvasId) -> bool {
        // SAFETY: The host outlives this.
        unsafe { (self.0.webgl_can_copy_images_into_textures)(self.host(), client.0, canvas_id) }
    }

    fn held_video_frame(
        &self,
        client: WebContentClientId,
        sink_handle: VideoSinkHandle,
        generation: u64,
    ) -> Option<(CppObject, IntSize)> {
        let mut frame = FfiCppObject::default();
        let mut size = IntSize::default();
        // SAFETY: The host outlives this, and writes the frame and its size where it answers true.
        let held = unsafe {
            (self.0.held_video_frame)(
                self.host(),
                client.0,
                sink_handle.0,
                generation,
                &raw mut frame,
                &raw mut size,
            )
        };
        if !held {
            return None;
        }
        // SAFETY: The host handed the frame over.
        unsafe { CppObject::adopt(frame) }.map(|frame| (frame, size))
    }

    fn video_frame_image(&self, frame: &CppObject) -> Option<CppObject> {
        let mut image = FfiCppObject::default();
        // SAFETY: The host outlives this, the frame is one it made, and it writes an image or null.
        unsafe { (self.0.video_frame_image)(self.host(), frame.as_ptr(), &raw mut image) };
        // SAFETY: The host handed the image over, if there is one.
        unsafe { CppObject::adopt(image) }
    }

    fn copy_video_frame_image_into_bound_texture(
        &self,
        client: WebContentClientId,
        canvas_id: CanvasId,
        image: &CppObject,
        copy: VideoFrameCopy,
    ) -> bool {
        // SAFETY: The host outlives this, and the image is one it made.
        unsafe {
            (self.0.copy_video_frame_image_into_bound_texture)(self.host(), client.0, canvas_id, image.as_ptr(), copy)
        }
    }

    fn convert_image_source(
        &self,
        client: WebContentClientId,
        source: ImageSource,
        conversion: ImageConversion,
    ) -> ImageConversionOutcome<ConvertedPixels> {
        let source = match source {
            ImageSource::RegisteredImageFrame(id) => FfiImageSource {
                is_held_video_frame: false,
                id: id.0,
                generation: 0,
            },
            ImageSource::HeldVideoFrame {
                sink_handle,
                generation,
            } => FfiImageSource {
                is_held_video_frame: true,
                id: sink_handle.0,
                generation,
            },
        };
        let conversion = FfiImageConversion {
            export_format: conversion.export_format as u8,
            flip_y: conversion.flip_y,
            premultiply_alpha: conversion.premultiply_alpha,
            has_target_size: conversion.target_size.is_some(),
            target_size: conversion.target_size.unwrap_or_default(),
        };
        let mut converted = FfiConvertedPixels {
            pixels: FfiCppObject::default(),
            data: std::ptr::null(),
            size: 0,
            width: 0,
            height: 0,
        };
        // SAFETY: The host outlives this, and writes the converted pixels where it answers that it converted them.
        let outcome = unsafe {
            (self.0.convert_image_source_for_webgl)(self.host(), client.0, source, conversion, &raw mut converted)
        };
        match outcome {
            FfiImageConversionOutcome::Converted => {
                // SAFETY: The host handed the pixels over.
                let owner = unsafe { CppObject::adopt(converted.pixels) }.expect("converted pixels have an owner");
                ImageConversionOutcome::Converted {
                    pixels: ConvertedPixels {
                        _owner: owner,
                        data: converted.data,
                        size: converted.size,
                    },
                    width: converted.width,
                    height: converted.height,
                }
            }
            FfiImageConversionOutcome::UnknownImageFrame => ImageConversionOutcome::UnknownImageFrame,
            FfiImageConversionOutcome::Failed => ImageConversionOutcome::Failed,
        }
    }
}

pub struct Canvas2DPainting<'a> {
    raster: &'a RasterHost,
    painter: FfiCanvas2DPainter,
}

impl Canvas2DPainting<'_> {
    pub fn play_command(&self, command_type: DisplayListCommandType, payload: &[u8]) {
        // SAFETY: The painting has not ended, and the player reads the payload during the call.
        unsafe { (self.painter.play_command)(self.painter.player, command_type, ak::Span::new(payload)) };
    }

    pub fn end_reporting_whether_video_frames_were_sampled_on_the_gpu(self) -> bool {
        // SAFETY: The host outlives this.
        unsafe { (self.raster.0.end_canvas_2d_painting)(self.raster.host()) }
    }
}

impl RemoteLayerHost for RasterHost {
    fn create_window_server(&self) -> Option<WindowServerObject> {
        let mut window_server = FfiCppObject::default();
        // SAFETY: The host outlives this, and writes the window server it made.
        if !unsafe { (self.0.create_window_server)(self.host(), &raw mut window_server) } {
            return None;
        }
        // SAFETY: The host handed a window server over.
        unsafe { WindowServerObject::adopt(window_server) }
    }

    fn create_remote_layer(
        &self,
        window_server: &WindowServerObject,
        remote_layer_id: u64,
    ) -> Option<(RemoteLayerObject, Compositing_RemoteLayerHostHandle)> {
        let mut layer = FfiCppObject::default();
        let mut host_handle = FfiRemoteLayerHostHandle::default();
        // SAFETY: The window server is live, and the host writes the layer it made with its handle.
        if !unsafe {
            (self.0.create_remote_layer)(
                window_server.as_ptr(),
                remote_layer_id,
                &raw mut layer,
                &raw mut host_handle,
            )
        } {
            return None;
        }
        // SAFETY: The host handed the layer over.
        let layer = unsafe { RemoteLayerObject::adopt(layer) }?;
        let kind = match host_handle.kind {
            1 => Compositing_RemoteLayerHostKind::CoreAnimationContext,
            2 => Compositing_RemoteLayerHostKind::Win32ChildWindow,
            _ => return None,
        };
        Some((
            layer,
            Compositing_RemoteLayerHostHandle {
                kind,
                id: host_handle.id,
            },
        ))
    }

    fn place_frame(&self, remote_layer: &RemoteLayerObject, fit_within: Option<IntSize>) {
        // SAFETY: The layer is live.
        unsafe {
            (self.0.remote_layer_place_frame)(
                remote_layer.as_ptr(),
                fit_within.is_some(),
                fit_within.unwrap_or_default(),
            );
        }
    }

    fn hold_changes_on_screen(&self, window_server: &WindowServerObject) {
        // SAFETY: The window server is live.
        unsafe { (self.0.window_server_hold_changes_on_screen)(window_server.as_ptr()) };
    }

    fn release_changes_held_off_screen(&self, window_server: &WindowServerObject) {
        // SAFETY: The window server is live.
        unsafe { (self.0.window_server_release_changes_held_off_screen)(window_server.as_ptr()) };
    }
}
