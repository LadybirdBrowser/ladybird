/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The window server: the compositor of the system, which shows the remote layers that this process commits frames
//! into, and which shows by itself what a frame leaves to it, such as the tiles of painted layers, canvases and videos,
//! instead of having them composited into the frame. A presentation has one window server, and each view it shows a
//! root context in has a remote layer of it.

mod cpp_backed;

use crate::cpp_object::CppObject;
use crate::raster_host::{FrameHandOff, LiveContent, RasterHost};
use crate::surfaces::presentable_surface::{PresentableSurface, PresentableSurfaceLease};
use libcompositing_rust::display_list::layers::WindowServerCompositing;
use libcompositing_rust::display_list::window_server_placement::{WindowServerBackdropFilters, WindowServerPlacement};
use libcompositing_rust::ipc_types::Compositing_RemoteLayerHostHandle;
use libgfx_rust::{AffineTransform, Color, CornerRadii, FloatRect, IntRect, IntSize};
use std::ffi::c_void;
use std::rc::Rc;

/// The window server of a presentation.
pub trait WindowServer {
    /// What it composites by itself of the layers of the frames its remote layers show.
    fn compositing(&self) -> WindowServerCompositing;
    /// When a frame shown in one of its remote layers can be handed to it.
    fn frame_hand_off(&self) -> FrameHandOff;
    fn create_remote_layer(&self) -> Option<Rc<dyn RemoteLayer>>;
    /// While held, what every remote layer of the window server shows changes nowhere on screen, and changes all at
    /// once when released.
    fn hold_changes_on_screen(&self);
    fn release_changes_held_off_screen(&self);
}

/// A layer that this process commits frames into, and that the presentation shows in a view of its own by the layer's
/// host handle, so that frames reach the screen without passing through the presentation.
pub trait RemoteLayer {
    fn host_handle(&self) -> Compositing_RemoteLayerHostHandle;
    /// Whether the layer can show the media as it is, placed and clipped as the frame would draw it.
    fn can_show(&self, placement: &WindowServerPlacement, media: MediaToShow<'_>, device_pixel_ratio: f64) -> bool;
    /// Shows the part of the store that the frame was painted into, at the top left of the layer at its own size, or
    /// scaled to fit within the size of the view given, in device pixels, with the sublayers above it, bottom to top,
    /// each group with the sublayers in it. Answers false where the layer could not show the frame.
    #[must_use]
    fn show_frame(&self, frame: Frame) -> bool;
    /// Places the frame on display again, as when the view that it is scaled into changes size.
    fn place_frame(&self, fit_within: Option<IntSize>);
}

/// The window server of a new presentation, or none where frames reach the screen only through the presentation.
pub fn create_for_presentation(raster: &RasterHost) -> Option<Rc<dyn WindowServer>> {
    cpp_backed::CppBackedWindowServer::create(raster)
}

/// Media that a remote layer is asked whether it can show as it is.
#[derive(Clone, Copy)]
pub enum MediaToShow<'a> {
    Surface(&'a Rc<PresentableSurface>),
    Video(&'a VideoFrameForWindowServer),
}

/// A frame of a video that the window server can show, which the decoder does not decode into again for as long as it
/// is held.
pub struct VideoFrameForWindowServer {
    frame: CppObject,
}

impl VideoFrameForWindowServer {
    pub fn new(frame: CppObject) -> Self {
        Self { frame }
    }

    pub fn as_ffi(&self) -> *mut c_void {
        self.frame.as_ptr()
    }
}

impl std::fmt::Debug for VideoFrameForWindowServer {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("VideoFrameForWindowServer")
            .field("frame", &self.frame.as_ptr())
            .finish()
    }
}

/// Holds the sublayers that name it as their group, which the window server places, clips and fades together.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Group {
    /// From the pixels of the group to those of the group it is in, or to those of the frame.
    pub to_parent: AffineTransform,
    /// Clips the sublayers of the group to this rect of its own pixels, with its corners rounded by the radii.
    pub clip_rect: Option<FloatRect>,
    pub corner_radii: CornerRadii,
    /// Composites the sublayers of the group at this opacity, once they are composited with each other.
    pub opacity: f32,
}

impl Default for Group {
    fn default() -> Self {
        Self {
            to_parent: AffineTransform::identity(),
            clip_rect: None,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        }
    }
}

/// What the window server shows below a sublayer, within `region` of the pixels of the group the sublayer is in, run
/// through the filters.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Backdrop {
    pub region: FloatRect,
    pub filters: WindowServerBackdropFilters,
}

/// What a sublayer shows: a group of other sublayers, a tile of a painted layer or a fill, both at a rect of the pixels
/// of their group, a backdrop, or content placed and clipped as drawing it into the frame would place and clip it. A
/// sublayer holds its surfaces as `Surface` and its placed content as `Placed`.
#[derive(Clone, Debug, PartialEq)]
pub enum SublayerContent<Surface, Placed> {
    Group(Group),
    Tile {
        surface: Surface,
        rect: IntRect,
        /// What of the surface shows at `rect`. Around it, the surface of a tile holds a border of what lies beside the
        /// tile, which a filter blends the tile's edges with where the tile is shown scaled.
        rect_in_surface: IntRect,
        every_pixel_is_opaque: bool,
    },
    Color {
        color: Color,
        rect: IntRect,
    },
    Backdrop(Backdrop),
    Placed {
        content: Placed,
        placement: WindowServerPlacement,
    },
}

/// Content that the window server shows of a frame by itself. Geometry is in device pixels.
#[derive(Clone, Debug, PartialEq)]
pub struct Sublayer<Surface, Placed> {
    /// Tells a sublayer apart from the others from one frame to the next, so that a frame changes only what moved.
    pub identity: u64,
    /// The index, among the sublayers of the frame, of the group the sublayer is in, which comes before it. Sublayers
    /// of one group, and those of the frame itself, which have none, are shown bottom to top.
    pub group_index: Option<usize>,
    pub content: SublayerContent<Surface, Placed>,
}

/// Placed content as a frame plans it: a canvas of the context by its id, a video by the handle of its sink, or a
/// surface.
#[derive(Clone, Debug, PartialEq)]
pub enum PlannedContent {
    Canvas(u64),
    Video(u64),
    Surface(Rc<PresentableSurface>),
}

/// A sublayer as a frame plans it, which tells whether a frame shows the same sublayers as the frame before.
pub type PlannedSublayer = Sublayer<Rc<PresentableSurface>, PlannedContent>;

/// Placed content of a frame on its way to the window server.
#[derive(Clone, Debug)]
pub enum PlacedContent {
    Surface(PresentableSurfaceLease),
    Video(Rc<VideoFrameForWindowServer>),
}

/// A sublayer of a frame on its way to the window server, which keeps the content of its surfaces as it is.
pub type FrameSublayer = Sublayer<PresentableSurfaceLease, PlacedContent>;

/// A frame that a remote layer shows.
pub struct Frame {
    /// Without a store, the store shown before stays, and only the sublayers change, unless the frame shows no store.
    pub store: Option<PresentableSurfaceLease>,
    /// Whether the sublayers are all of the frame, which then shows no store below them.
    pub shows_no_store: bool,
    pub painted_size: IntSize,
    pub device_pixel_ratio: f64,
    /// Where the frame is scaled to fit within the view, in device pixels, rather than shown at its own size.
    pub fit_within: Option<IntSize>,
    pub sublayers: Vec<FrameSublayer>,
}

/// The sublayers of a frame on its way to the window server, with the media of the planned ones as the live content of
/// the frame has them. A sublayer whose media is gone is left out, and so is every sublayer of a group that is.
pub fn sublayers_for_frame(planned: &[PlannedSublayer], live_content: &LiveContent) -> Vec<FrameSublayer> {
    let mut sublayers = Vec::with_capacity(planned.len());
    let mut index_of_sublayer_for_frame: Vec<Option<usize>> = Vec::with_capacity(planned.len());
    for sublayer in planned {
        let group_index = match sublayer.group_index {
            None => None,
            // A group comes before the sublayers in it.
            Some(group_index) => match index_of_sublayer_for_frame.get(group_index).copied().flatten() {
                Some(index) => Some(index),
                None => {
                    index_of_sublayer_for_frame.push(None);
                    continue;
                }
            },
        };
        let content = match &sublayer.content {
            SublayerContent::Group(group) => Some(SublayerContent::Group(*group)),
            SublayerContent::Tile {
                surface,
                rect,
                rect_in_surface,
                every_pixel_is_opaque,
            } => Some(SublayerContent::Tile {
                surface: surface.lease(),
                rect: *rect,
                rect_in_surface: *rect_in_surface,
                every_pixel_is_opaque: *every_pixel_is_opaque,
            }),
            SublayerContent::Color { color, rect } => Some(SublayerContent::Color {
                color: *color,
                rect: *rect,
            }),
            SublayerContent::Backdrop(backdrop) => Some(SublayerContent::Backdrop(*backdrop)),
            SublayerContent::Placed { content, placement } => {
                let content = match content {
                    PlannedContent::Canvas(id) => live_content
                        .canvases
                        .get(id)
                        .and_then(|canvas| canvas.presentable_surface.as_ref())
                        .map(|surface| PlacedContent::Surface(surface.lease())),
                    PlannedContent::Video(handle) => live_content
                        .video_sinks
                        .get(handle)
                        .filter(|sink| sink.frame_has_surface)
                        .and_then(|sink| sink.frame.clone())
                        .map(PlacedContent::Video),
                    PlannedContent::Surface(surface) => Some(PlacedContent::Surface(surface.lease())),
                };
                content.map(|content| SublayerContent::Placed {
                    content,
                    placement: placement.clone(),
                })
            }
        };
        let Some(content) = content else {
            index_of_sublayer_for_frame.push(None);
            continue;
        };
        index_of_sublayer_for_frame.push(Some(sublayers.len()));
        sublayers.push(FrameSublayer {
            identity: sublayer.identity,
            group_index,
            content,
        });
    }
    sublayers
}
