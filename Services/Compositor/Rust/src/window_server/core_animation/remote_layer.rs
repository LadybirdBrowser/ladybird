/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A CoreAnimation layer tree that this process commits frames into, and that another process shows in a view of its
//! own by the ID of the tree's context.

use super::frameworks::{CGAffineTransform, CGColorCreateSRGB, CGPoint, CGRect, CfRetained};
use super::objc::Id;
use super::path::{outline_of_path, outline_of_rect_with_rounded_corners};
use super::quartz_core::{
    Context, Layer, MAX_X_MAX_Y_CORNER, MAX_X_MIN_Y_CORNER, MIN_X_MAX_Y_CORNER, MIN_X_MIN_Y_CORNER,
    SampleBufferDisplayLayer, ScalingFilter, ShapeLayer, filter_of_type, set_value_for_key, value_with_bytes,
};
use super::video::{MATRIX_COEFFICIENTS_IDENTITY, describe_colours_of_video_surface, enqueue_video_surface};
use super::{changes_are_held_off_screen, changes_last_released_on_screen, commit_layer_changes, size_of_io_surface};
use crate::presenting::rect_of_size;
use crate::surfaces::presentable_surface::PresentableSurfaceLease;
use crate::window_server::shown_sublayers::{NativeSublayers, ShownSublayers, SublayerKind};
use crate::window_server::{
    Backdrop, Frame, Group, MediaToShow, PlacedContent, RemoteLayer, SublayerContent, VideoFrameForWindowServer,
    rect_fitting_frame,
};
use libcompositing_rust::display_list::window_server_placement::{RoundedCorners, WindowServerPlacement};
use libcompositing_rust::ipc_types::{Compositing_RemoteLayerHostHandle, Compositing_RemoteLayerHostKind};
use libgfx_rust::{AffineTransform, Color, CornerRadii, FloatRect, IntRect, IntSize, ScalingMode, WindingRule};
use std::cell::RefCell;
use std::mem::ManuallyDrop;
use std::rc::Rc;
use std::time::{Duration, Instant};

fn in_points(rect: FloatRect, device_pixel_ratio: f64) -> CGRect {
    CGRect::new(
        f64::from(rect.x) / device_pixel_ratio,
        f64::from(rect.y) / device_pixel_ratio,
        f64::from(rect.width) / device_pixel_ratio,
        f64::from(rect.height) / device_pixel_ratio,
    )
}

fn int_rect_in_points(rect: IntRect, device_pixel_ratio: f64) -> CGRect {
    in_points(rect.to_float(), device_pixel_ratio)
}

fn to_core_graphics(transform: AffineTransform) -> CGAffineTransform {
    let [a, b, c, d, e, f] = transform.values.map(f64::from);
    CGAffineTransform::new(a, b, c, d, e, f)
}

/// The part of a surface that a layer shows, as a unit rectangle, which turns upside down along with the layer's y-axis.
fn unit_rect_of_surface_part(layer: &Layer, part: IntRect, surface_size: IntSize) -> CGRect {
    let unit_x = f64::from(part.x) / f64::from(surface_size.width);
    let unit_y = f64::from(part.y) / f64::from(surface_size.height);
    let unit_width = f64::from(part.width) / f64::from(surface_size.width);
    let unit_height = f64::from(part.height) / f64::from(surface_size.height);
    if layer.contents_are_flipped() {
        CGRect::new(unit_x, unit_y, unit_width, unit_height)
    } else {
        CGRect::new(unit_x, 1.0 - unit_y - unit_height, unit_width, unit_height)
    }
}

fn filter_for_scaling_mode(scaling_mode: ScalingMode) -> ScalingFilter {
    match scaling_mode {
        ScalingMode::None | ScalingMode::NearestNeighbor => ScalingFilter::Nearest,
        ScalingMode::Bilinear | ScalingMode::BilinearMipmap => ScalingFilter::Linear,
    }
}

/// The corners of a layer named in its own coordinates, whose y-axis points down here like the frame's.
fn masked_corners(top_left: bool, top_right: bool, bottom_right: bool, bottom_left: bool) -> usize {
    let mut corners = 0;
    if top_left {
        corners |= MIN_X_MIN_Y_CORNER;
    }
    if top_right {
        corners |= MAX_X_MIN_Y_CORNER;
    }
    if bottom_right {
        corners |= MAX_X_MAX_Y_CORNER;
    }
    if bottom_left {
        corners |= MIN_X_MAX_Y_CORNER;
    }
    corners
}

/// AVFoundation tone maps PQ and HLG video differently from how the window server maps the same surface as a layer's
/// contents, and it is how the system's own players show such video, so it shows HDR video here too.
fn is_shown_through_sample_buffers(content: &PlacedContent) -> bool {
    matches!(content, PlacedContent::Video(frame) if frame.color.holds_high_dynamic_range())
}

/// The layer that shows placed content.
enum ContentLayer {
    Plain(Layer),
    SampleBuffers(SampleBufferDisplayLayer),
}

impl ContentLayer {
    fn layer(&self) -> &Layer {
        match self {
            ContentLayer::Plain(layer) => layer,
            ContentLayer::SampleBuffers(layer) => layer.layer(),
        }
    }
}

enum ShownContent {
    None,
    Surface(PresentableSurfaceLease),
    Video(Rc<VideoFrameForWindowServer>),
}

/// What a frame shows a sublayer with.
struct Sublayer {
    /// The layer among the sublayers of the group the sublayer is in, or of the frame: the layer that clips placed
    /// content, or the one layer of a group, a tile, a color or a backdrop.
    outermost_layer: Layer,
    rounded_clip_layer: Option<Layer>,
    /// The mask of the outermost layer of placed content clipped by a path, with the identity of the path its outline
    /// was made of, and whether it fills by the even-odd rule.
    path_clip_mask: Option<(ShapeLayer, u64, bool)>,
    /// The mask of a group whose corners are rounded by more than one radius, or by elliptical ones.
    rounded_corners_mask: Option<ShapeLayer>,
    content_layer: Option<ContentLayer>,
    content: ShownContent,
    /// What a group, a tile or a color was last shown with, so that a frame sets only what it changes.
    shown_group: Option<Group>,
    shown_rect: Option<IntRect>,
    shown_color: Option<Color>,
    shown_as_opaque: Option<bool>,
    shown_backdrop: Option<Backdrop>,
    shown_at_device_pixel_ratio: f64,
}

/// A decoder decodes into the surface of a frame again once nothing holds the frame, so a replaced frame is held until
/// the window server can no longer be reading it. Its surface reads as in use for as long as the decoder's pixel buffer
/// lives, which is as long as the frame does, so that cannot tell.
struct ReplacedVideoFrame {
    _frame: Rc<VideoFrameForWindowServer>,
    replaced_at: Instant,
}

/// The window server shows a replacement from its next display refresh on, and a composite that started before may
/// still be reading the frame replaced. A few refreshes later, even at 60 Hz, it has let go of it.
const TIME_WINDOW_SERVER_MAY_READ_A_REPLACED_SURFACE: Duration = Duration::from_millis(50);
/// How soon replaced frames are looked at again while the window server holds changes off screen.
const TIME_BETWEEN_LOOKS_AT_REPLACED_FRAMES_WHILE_CHANGES_ARE_HELD: Duration = Duration::from_millis(25);

/// The layers of the frame, and what the sublayers of the frame are shown with.
struct Natives {
    frame_layer: Layer,
    content_layer: Layer,
    device_pixel_ratio: f64,
    replaced_video_frames: Vec<ReplacedVideoFrame>,
}

struct State {
    context: Context,
    root_layer: Layer,
    painted_size: IntSize,
    sublayers: ShownSublayers<Sublayer>,
    natives: Natives,
}

pub(super) struct CoreAnimationRemoteLayer {
    context_id: u32,
    // Dropped inside a transaction of its own, as the layers and the context are released.
    state: ManuallyDrop<RefCell<State>>,
}

impl CoreAnimationRemoteLayer {
    pub(super) fn create() -> Option<Self> {
        commit_layer_changes(|| {
            let root_layer = Layer::new();
            // Frames are laid out from the top left, as the view that hosts the tree is.
            root_layer.set_geometry_flipped(true);
            // The frame and the sublayers over it are laid out in the frame's own pixels, and scaled together to fit.
            let frame_layer = Layer::new();
            root_layer.add_sublayer(&frame_layer);
            let content_layer = Layer::new();
            frame_layer.add_sublayer(&content_layer);
            let context = Context::create()?;
            context.set_layer(Some(&root_layer));
            let context_id = context.id();
            if context_id == 0 {
                context.set_layer(None);
                return None;
            }
            Some(Self {
                context_id,
                state: ManuallyDrop::new(RefCell::new(State {
                    context,
                    root_layer,
                    painted_size: IntSize::default(),
                    sublayers: ShownSublayers::default(),
                    natives: Natives {
                        frame_layer,
                        content_layer,
                        device_pixel_ratio: 1.0,
                        replaced_video_frames: Vec::new(),
                    },
                })),
            })
        })
    }
}

impl Drop for CoreAnimationRemoteLayer {
    fn drop(&mut self) {
        // SAFETY: The state is taken once, as the layer is dropped, and not used after.
        let mut state = unsafe { ManuallyDrop::take(&mut self.state) }.into_inner();
        commit_layer_changes(move || {
            state.sublayers.stop_showing_all(&mut state.natives);
            state.context.set_layer(None);
            drop(state);
        });
    }
}

impl State {
    /// The root layer spans the view a frame is fitted into, as its y-axis is flipped within its own height.
    fn lay_out_frame(&self, fit_within: Option<IntSize>) {
        let device_pixel_ratio = self.natives.device_pixel_ratio;
        let frame_rect = rect_of_size(self.painted_size);
        let root_rect = rect_of_size(fit_within.unwrap_or(self.painted_size));
        let fitted_rect = fit_within.map_or(root_rect, |fit_within| {
            rect_fitting_frame(self.painted_size, fit_within)
        });
        self.root_layer
            .set_frame(int_rect_in_points(root_rect, device_pixel_ratio));
        let frame_layer = &self.natives.frame_layer;
        frame_layer.set_bounds(int_rect_in_points(frame_rect, device_pixel_ratio));
        frame_layer.set_position(int_rect_in_points(fitted_rect, device_pixel_ratio).origin);
        frame_layer.set_affine_transform(CGAffineTransform::scale(
            f64::from(fitted_rect.width) / f64::from(frame_rect.width.max(1)),
            f64::from(fitted_rect.height) / f64::from(frame_rect.height.max(1)),
        ));
        self.natives
            .content_layer
            .set_frame(int_rect_in_points(frame_rect, device_pixel_ratio));
    }
}

impl RemoteLayer for CoreAnimationRemoteLayer {
    fn host_handle(&self) -> Compositing_RemoteLayerHostHandle {
        Compositing_RemoteLayerHostHandle {
            kind: Compositing_RemoteLayerHostKind::CoreAnimationContext,
            id: u64::from(self.context_id),
        }
    }

    fn can_show(&self, _: &WindowServerPlacement, media: MediaToShow<'_>) -> bool {
        match media {
            MediaToShow::Surface(surface) => surface.io_surface().is_some(),
            // A frame that a decoder left in memory has no surface that the window server could read. The window
            // server converts a surface from YCbCr by the matrix it is tagged with, and no tag names the identity
            // matrix that an RGB surface needs.
            MediaToShow::Video(frame) => {
                !frame.surface().is_null() && frame.color.matrix_coefficients != MATRIX_COEFFICIENTS_IDENTITY
            }
        }
    }

    fn show_frame(&self, frame: Frame) -> bool {
        let mut state = self.state.borrow_mut();
        let state = &mut *state;
        let mut store = None;
        if let Some(lease) = &frame.store {
            let surface = lease.surface();
            let Some(io_surface) = surface.io_surface() else {
                return false;
            };
            surface.did_hand_to_window_server();
            store = Some(io_surface);
        }
        let mut painted_size = frame.painted_size;
        let mut surface_size = IntSize::default();
        if let Some(io_surface) = store {
            surface_size = size_of_io_surface(io_surface);
            // A store can be larger than the frame painted into it, as while the window is resized.
            painted_size = IntSize {
                width: painted_size.width.min(surface_size.width),
                height: painted_size.height.min(surface_size.height),
            };
            if painted_size.is_empty() {
                return true;
            }
            state.painted_size = painted_size;
        }
        if frame.shows_no_store {
            if painted_size.is_empty() {
                return true;
            }
            state.painted_size = painted_size;
        }
        if state.painted_size.is_empty() {
            return true;
        }
        let device_pixel_ratio = frame.device_pixel_ratio;
        state.natives.device_pixel_ratio = device_pixel_ratio;

        commit_layer_changes(|| {
            let content_layer = &state.natives.content_layer;
            if store.is_some() {
                content_layer.set_contents_rect(unit_rect_of_surface_part(
                    content_layer,
                    rect_of_size(state.painted_size),
                    surface_size,
                ));
                content_layer.set_contents_scale(device_pixel_ratio);
            }
            state.lay_out_frame(frame.fit_within);
            let content_layer = &state.natives.content_layer;
            if let Some(io_surface) = store {
                content_layer.set_contents(io_surface.cast_const());
            } else if frame.shows_no_store && content_layer.has_contents() {
                content_layer.set_contents(std::ptr::null());
            }
            state.sublayers.show(&mut state.natives, frame.sublayers);
        });
        true
    }

    fn place_frame(&self, fit_within: Option<IntSize>) {
        let state = self.state.borrow();
        if state.painted_size.is_empty() {
            return;
        }
        commit_layer_changes(|| state.lay_out_frame(fit_within));
    }

    fn release_content_window_server_let_go_of(&self) -> Option<Duration> {
        self.state
            .borrow_mut()
            .natives
            .release_video_frames_window_server_let_go_of()
    }
}

impl NativeSublayers for Natives {
    type Native = Sublayer;

    fn create(&mut self, kind: SublayerKind) -> Option<Sublayer> {
        let outermost_layer = if kind == SublayerKind::Backdrop {
            Layer::new_backdrop()?
        } else {
            Layer::new()
        };
        // A group fades what its sublayers show together, not each of them over the ones below it.
        if kind == SublayerKind::Group {
            outermost_layer.set_allows_group_opacity(true);
        }
        let meets_other_sublayers_edge_to_edge = matches!(kind, SublayerKind::Tile | SublayerKind::Color);
        if meets_other_sublayers_edge_to_edge {
            outermost_layer.stop_antialiasing_edges();
        }
        let mut content_layer = None;
        if kind == SublayerKind::PlacedContent {
            outermost_layer.set_masks_to_bounds(true);
            let layer = Layer::new();
            outermost_layer.add_sublayer(&layer);
            content_layer = Some(ContentLayer::Plain(layer));
        }
        Some(Sublayer {
            outermost_layer,
            rounded_clip_layer: None,
            path_clip_mask: None,
            rounded_corners_mask: None,
            content_layer,
            content: ShownContent::None,
            shown_group: None,
            shown_rect: None,
            shown_color: None,
            shown_as_opaque: None,
            shown_backdrop: None,
            shown_at_device_pixel_ratio: 0.0,
        })
    }

    fn update(&mut self, native: &mut Sublayer, content: SublayerContent<PresentableSurfaceLease, PlacedContent>) {
        match content {
            SublayerContent::Group(group) => self.update_shown_group(native, &group),
            SublayerContent::Tile {
                surface,
                rect,
                rect_in_surface,
                every_pixel_is_opaque,
            } => self.update_shown_tile(native, surface, rect, rect_in_surface, every_pixel_is_opaque),
            SublayerContent::Color { color, rect } => self.update_shown_color(native, color, rect),
            SublayerContent::Backdrop(backdrop) => self.update_shown_backdrop(native, &backdrop),
            SublayerContent::Placed { content, placement } => self.update_placed_content(native, content, &placement),
        }
        native.shown_at_device_pixel_ratio = self.device_pixel_ratio;
    }

    fn stop_showing(&mut self, mut native: Sublayer) {
        if native.content_layer.is_some() {
            self.stop_showing_content(&mut native);
        }
        native.content = ShownContent::None;
        native.outermost_layer.remove_from_superlayer();
    }

    // A layer given as a sublayer leaves the layer it was in, so nothing has to be taken out first.
    fn remove_sublayers_of_group(&mut self, _: &Sublayer) {}

    fn remove_sublayers_of_frame(&mut self) {}

    fn add_sublayers_to_group(&mut self, group: &Sublayer, sublayers: &[&Sublayer]) {
        let layers: Vec<&Layer> = sublayers.iter().map(|sublayer| &sublayer.outermost_layer).collect();
        group.outermost_layer.set_sublayers(&layers);
    }

    fn add_sublayers_to_frame(&mut self, sublayers: &[&Sublayer]) {
        let mut layers: Vec<&Layer> = Vec::with_capacity(sublayers.len() + 1);
        layers.push(&self.content_layer);
        layers.extend(sublayers.iter().map(|sublayer| &sublayer.outermost_layer));
        self.frame_layer.set_sublayers(&layers);
    }
}

fn places_alike(a: &Group, b: &Group) -> bool {
    a.to_parent.values == b.to_parent.values && a.clip_rect == b.clip_rect
}

fn rounds_corners_alike(a: &Group, b: &Group) -> bool {
    let size_clipped_to = |group: &Group| group.clip_rect.map(|rect| (rect.width, rect.height));
    a.corner_radii == b.corner_radii && size_clipped_to(a) == size_clipped_to(b)
}

/// The one radius that every rounded corner has, or none where two rounded corners differ or one is elliptical.
fn single_radius_of_rounded_corners(corner_radii: &CornerRadii) -> Option<i32> {
    let mut single_radius = None;
    for corner in [
        corner_radii.top_left,
        corner_radii.top_right,
        corner_radii.bottom_right,
        corner_radii.bottom_left,
    ] {
        if !corner.is_present() {
            continue;
        }
        if corner.horizontal_radius != corner.vertical_radius
            || single_radius.unwrap_or(corner.horizontal_radius) != corner.horizontal_radius
        {
            return None;
        }
        single_radius = Some(corner.horizontal_radius);
    }
    Some(single_radius.unwrap_or(0))
}

/// The window server blurs by a standard deviation in points, and fills in what lies past the edge of a backdrop by
/// normalizing, which comes closest to mirroring at the edge. A CAColorMatrix holds the same 20 floats in the same
/// order, and the window server applies it to unpremultiplied colours as well.
fn core_animation_filters_of(backdrop: &Backdrop, device_pixel_ratio: f64) -> Vec<Id> {
    let filters = &backdrop.filters;
    let mut core_animation_filters = Vec::with_capacity(3);
    let append_color_matrix = |color_matrix: &[f32; 20], filters: &mut Vec<Id>| {
        let Some(filter) = filter_of_type(c"colorMatrix") else {
            return;
        };
        let Some(value) = value_with_bytes(color_matrix.as_ptr().cast(), c"{CAColorMatrix=ffffffffffffffffffff}")
        else {
            return;
        };
        set_value_for_key(&filter, value.as_ptr(), c"inputColorMatrix");
        filters.push(filter);
    };
    if let Some(color_matrix) = &filters.color_matrix_before_blur {
        append_color_matrix(color_matrix, &mut core_animation_filters);
    }
    if let Some(standard_deviation) = filters.blur_standard_deviation
        && let Some(blur) = filter_of_type(c"gaussianBlur")
    {
        let radius = CfRetained::number(f64::from(standard_deviation) / device_pixel_ratio);
        set_value_for_key(&blur, radius.as_ptr(), c"inputRadius");
        // SAFETY: kCFBooleanTrue is a constant of CoreFoundation.
        set_value_for_key(
            &blur,
            unsafe { super::frameworks::kCFBooleanTrue },
            c"inputNormalizeEdges",
        );
        core_animation_filters.push(blur);
    }
    if let Some(color_matrix) = &filters.color_matrix_after_blur {
        append_color_matrix(color_matrix, &mut core_animation_filters);
    }
    core_animation_filters
}

impl Natives {
    /// A group layer has the rect the group clips to as its bounds, or no bounds at its origin, so that its sublayers
    /// are laid out in the pixels of the group. It is placed by where the origin of those bounds goes, with the rest of
    /// the group's transform around that point. A frame that only moves the group, as a scroll does, sets nothing
    /// else.
    fn update_shown_group(&mut self, native: &mut Sublayer, group: &Group) {
        let device_pixel_ratio = self.device_pixel_ratio;
        let shown_group = native
            .shown_group
            .filter(|_| native.shown_at_device_pixel_ratio == device_pixel_ratio);
        let group_layer = &native.outermost_layer;
        let clip_rect = group.clip_rect.unwrap_or_default();
        let bounds = in_points(clip_rect, device_pixel_ratio);
        if shown_group.is_none_or(|shown_group| !places_alike(&shown_group, group)) {
            let to_parent = group.to_parent;
            let origin = to_parent.map_point(libgfx_rust::FloatPoint {
                x: clip_rect.x,
                y: clip_rect.y,
            });
            group_layer.set_bounds(bounds);
            group_layer.set_position(CGPoint {
                x: f64::from(origin.x) / device_pixel_ratio,
                y: f64::from(origin.y) / device_pixel_ratio,
            });
            group_layer.set_affine_transform(CGAffineTransform::new(
                f64::from(to_parent.a()),
                f64::from(to_parent.b()),
                f64::from(to_parent.c()),
                f64::from(to_parent.d()),
                0.0,
                0.0,
            ));
            group_layer.set_masks_to_bounds(group.clip_rect.is_some());
        }
        if shown_group.is_none_or(|shown_group| shown_group.opacity != group.opacity) {
            group_layer.set_opacity(group.opacity);
        }
        if shown_group.is_none_or(|shown_group| !rounds_corners_alike(&shown_group, group)) {
            // The window server rounds the corners of a layer by one radius. Corners that differ, or that are
            // elliptical, are cut by a mask of their outline instead, which costs it a pass of its own.
            let corner_radii = &group.corner_radii;
            let single_radius = single_radius_of_rounded_corners(corner_radii);
            group_layer.set_corner_radius(f64::from(single_radius.unwrap_or(0)) / device_pixel_ratio);
            group_layer.set_masked_corners(masked_corners(
                corner_radii.top_left.is_present(),
                corner_radii.top_right.is_present(),
                corner_radii.bottom_right.is_present(),
                corner_radii.bottom_left.is_present(),
            ));
            if single_radius.is_some() || group.clip_rect.is_none() {
                group_layer.set_mask(None);
                native.rounded_corners_mask = None;
            } else {
                let outline_layer = ShapeLayer::new();
                let outline = outline_of_rect_with_rounded_corners(bounds.size, corner_radii, device_pixel_ratio);
                outline_layer.set_path(outline.as_ptr());
                group_layer.set_mask(Some(outline_layer.layer()));
                native.rounded_corners_mask = Some(outline_layer);
            }
        }
        // The outline is laid out in the pixels of the group, as its sublayers are.
        if let Some(outline_layer) = &native.rounded_corners_mask {
            outline_layer.layer().set_frame(bounds);
        }
        native.shown_group = Some(*group);
    }

    fn update_shown_tile(
        &mut self,
        native: &mut Sublayer,
        surface: PresentableSurfaceLease,
        rect: IntRect,
        rect_in_surface: IntRect,
        every_pixel_is_opaque: bool,
    ) {
        let device_pixel_ratio = self.device_pixel_ratio;
        let tile_layer = &native.outermost_layer;
        if native.shown_rect != Some(rect) || native.shown_at_device_pixel_ratio != device_pixel_ratio {
            tile_layer.set_frame(int_rect_in_points(rect, device_pixel_ratio));
            native.shown_rect = Some(rect);
        }
        if native.shown_as_opaque != Some(every_pixel_is_opaque) {
            tile_layer.set_opaque(every_pixel_is_opaque);
            native.shown_as_opaque = Some(every_pixel_is_opaque);
        }
        if let ShownContent::Surface(shown) = &native.content
            && Rc::ptr_eq(shown.surface(), surface.surface())
        {
            return;
        }
        let Some(io_surface) = surface.surface().io_surface() else {
            return;
        };
        surface.surface().did_hand_to_window_server();
        tile_layer.set_contents_rect(unit_rect_of_surface_part(
            tile_layer,
            rect_in_surface,
            surface.surface().size(),
        ));
        tile_layer.set_contents(io_surface.cast_const());
        native.content = ShownContent::Surface(surface);
    }

    fn update_shown_color(&mut self, native: &mut Sublayer, color: Color, rect: IntRect) {
        let device_pixel_ratio = self.device_pixel_ratio;
        let color_layer = &native.outermost_layer;
        if native.shown_rect != Some(rect) || native.shown_at_device_pixel_ratio != device_pixel_ratio {
            color_layer.set_frame(int_rect_in_points(rect, device_pixel_ratio));
            native.shown_rect = Some(rect);
        }
        if native.shown_color == Some(color) {
            return;
        }
        // SAFETY: The answer is a new color the caller owns.
        let core_graphics_color = unsafe {
            CfRetained::adopt(CGColorCreateSRGB(
                f64::from(color.red()) / 255.0,
                f64::from(color.green()) / 255.0,
                f64::from(color.blue()) / 255.0,
                f64::from(color.alpha()) / 255.0,
            ))
        };
        color_layer.set_background_color(
            core_graphics_color
                .as_ref()
                .map_or(std::ptr::null(), CfRetained::as_ptr),
        );
        color_layer.set_opaque(color.alpha() == 255);
        native.shown_color = Some(color);
    }

    fn update_shown_backdrop(&mut self, native: &mut Sublayer, backdrop: &Backdrop) {
        let device_pixel_ratio = self.device_pixel_ratio;
        if native.shown_backdrop.as_ref() == Some(backdrop) && native.shown_at_device_pixel_ratio == device_pixel_ratio
        {
            return;
        }
        let backdrop_layer = &native.outermost_layer;
        backdrop_layer.set_frame(in_points(backdrop.region, device_pixel_ratio));
        backdrop_layer.set_filters(&core_animation_filters_of(backdrop, device_pixel_ratio));
        native.shown_backdrop = Some(*backdrop);
    }

    fn use_content_layer_of_kind_for(&mut self, native: &mut Sublayer, wants_sample_buffers: bool) {
        let shows_sample_buffers = matches!(native.content_layer, Some(ContentLayer::SampleBuffers(_)));
        if shows_sample_buffers == wants_sample_buffers {
            return;
        }
        let replacement = if wants_sample_buffers {
            let sample_buffer_layer = SampleBufferDisplayLayer::new();
            sample_buffer_layer.layer().set_opaque(true);
            sample_buffer_layer.layer().let_show_high_dynamic_range(true);
            // The 4 stops of headroom of an XDR display.
            sample_buffer_layer.layer().set_contents_headroom(16.0);
            ContentLayer::SampleBuffers(sample_buffer_layer)
        } else {
            ContentLayer::Plain(Layer::new())
        };
        self.stop_showing_content(native);
        if let Some(content_layer) = &native.content_layer
            && let Some(superlayer) = content_layer.layer().superlayer()
        {
            superlayer.replace_sublayer(content_layer.layer(), replacement.layer());
        }
        native.content_layer = Some(replacement);
    }

    fn update_placed_content(
        &mut self,
        native: &mut Sublayer,
        content: PlacedContent,
        placement: &WindowServerPlacement,
    ) {
        let device_pixel_ratio = self.device_pixel_ratio;
        self.use_content_layer_of_kind_for(native, is_shown_through_sample_buffers(&content));
        let clip_layer = native.outermost_layer.clone();
        let Some(content_layer) = native.content_layer.as_ref().map(|layer| layer.layer().clone()) else {
            return;
        };
        clip_layer.set_frame(in_points(placement.clip_rect, device_pixel_ratio));
        update_path_clip_mask(native, placement, device_pixel_ratio);

        // The content layer is placed relative to the innermost layer that clips it.
        let mut origin_of_content_parent = (placement.clip_rect.x, placement.clip_rect.y);
        if let Some(rounded_clip) = placement.rounded_clip {
            let rounded_clip_layer = native.rounded_clip_layer.get_or_insert_with(|| {
                let rounded_clip_layer = Layer::new();
                rounded_clip_layer.set_masks_to_bounds(true);
                clip_layer.add_sublayer(&rounded_clip_layer);
                rounded_clip_layer.add_sublayer(&content_layer);
                rounded_clip_layer
            });
            rounded_clip_layer.set_frame(in_points(
                rounded_clip
                    .rect
                    .translated(-origin_of_content_parent.0, -origin_of_content_parent.1),
                device_pixel_ratio,
            ));
            rounded_clip_layer.set_corner_radius(f64::from(rounded_clip.radius) / device_pixel_ratio);
            let RoundedCorners {
                top_left,
                top_right,
                bottom_right,
                bottom_left,
            } = rounded_clip.rounded_corners;
            rounded_clip_layer.set_masked_corners(masked_corners(top_left, top_right, bottom_right, bottom_left));
            origin_of_content_parent = (rounded_clip.rect.x, rounded_clip.rect.y);
        } else if let Some(rounded_clip_layer) = native.rounded_clip_layer.take() {
            clip_layer.add_sublayer(&content_layer);
            rounded_clip_layer.remove_from_superlayer();
        }

        // Maps the layer's points to the content's pixels, through the frame's pixels, to the points of its parent.
        let content_rect = placement.content_rect;
        let content_width_in_points = f64::from(content_rect.width) / device_pixel_ratio;
        let content_height_in_points = f64::from(content_rect.height) / device_pixel_ratio;
        let mut transform = CGAffineTransform::scale(device_pixel_ratio, device_pixel_ratio)
            .then(CGAffineTransform::translation(
                f64::from(content_rect.x),
                f64::from(content_rect.y),
            ))
            .then(to_core_graphics(placement.content_to_viewport))
            .then(CGAffineTransform::scale(
                1.0 / device_pixel_ratio,
                1.0 / device_pixel_ratio,
            ))
            .then(CGAffineTransform::translation(
                -f64::from(origin_of_content_parent.0) / device_pixel_ratio,
                -f64::from(origin_of_content_parent.1) / device_pixel_ratio,
            ));
        if let PlacedContent::Surface(lease) = &content
            && lease.surface().content_is_upside_down()
        {
            transform = CGAffineTransform::new(1.0, 0.0, 0.0, -1.0, 0.0, content_height_in_points).then(transform);
        }
        content_layer.set_bounds(CGRect::new(0.0, 0.0, content_width_in_points, content_height_in_points));
        content_layer.set_position(CGPoint { x: 0.0, y: 0.0 });
        content_layer.set_affine_transform(transform);
        let content_to_viewport = placement.content_to_viewport;
        content_layer.set_allows_edge_antialiasing(content_to_viewport.b() != 0.0 || content_to_viewport.c() != 0.0);
        content_layer.set_contents_scale(device_pixel_ratio);
        content_layer.set_magnification_filter(filter_for_scaling_mode(placement.scaling_mode));
        content_layer.set_minification_filter(if placement.scaling_mode == ScalingMode::BilinearMipmap {
            ScalingFilter::Trilinear
        } else {
            filter_for_scaling_mode(placement.scaling_mode)
        });

        self.show_content(native, content);
    }

    fn show_content(&mut self, native: &mut Sublayer, content: PlacedContent) {
        let Some(content_layer) = &native.content_layer else {
            return;
        };
        match content {
            PlacedContent::Video(frame) => {
                if let ShownContent::Video(shown_frame) = &native.content
                    && shown_frame.id == frame.id
                {
                    return;
                }
                let surface = frame.surface();
                if surface.is_null() {
                    return;
                }
                describe_colours_of_video_surface(surface, &frame.color);
                let surface_size = size_of_io_surface(surface);
                let shown_size = IntSize {
                    width: frame.visible_size.width.min(surface_size.width),
                    height: frame.visible_size.height.min(surface_size.height),
                };
                match content_layer {
                    ContentLayer::SampleBuffers(layer) => {
                        enqueue_video_surface(layer, surface, &frame.color, shown_size);
                    }
                    ContentLayer::Plain(layer) => {
                        layer.set_contents_rect(unit_rect_of_surface_part(
                            layer,
                            rect_of_size(shown_size),
                            surface_size,
                        ));
                        layer.set_opaque(true);
                        layer.set_contents(surface.cast_const());
                    }
                }
                let shown = std::mem::replace(&mut native.content, ShownContent::Video(frame));
                if let ShownContent::Video(shown_frame) = shown {
                    self.keep_video_frame_until_window_server_lets_go(shown_frame);
                }
            }
            PlacedContent::Surface(lease) => {
                if let ShownContent::Surface(shown) = &native.content
                    && Rc::ptr_eq(shown.surface(), lease.surface())
                {
                    return;
                }
                let Some(io_surface) = lease.surface().io_surface() else {
                    return;
                };
                lease.surface().did_hand_to_window_server();
                let layer = content_layer.layer();
                layer.set_contents_rect(CGRect::new(0.0, 0.0, 1.0, 1.0));
                layer.set_opaque(false);
                layer.set_contents(io_surface.cast_const());
                let shown = std::mem::replace(&mut native.content, ShownContent::Surface(lease));
                if let ShownContent::Video(shown_frame) = shown {
                    self.keep_video_frame_until_window_server_lets_go(shown_frame);
                }
            }
        }
    }

    fn stop_showing_content(&mut self, native: &mut Sublayer) {
        if let ShownContent::Video(shown_frame) = std::mem::replace(&mut native.content, ShownContent::None) {
            self.keep_video_frame_until_window_server_lets_go(shown_frame);
        }
        if let Some(ContentLayer::SampleBuffers(layer)) = &native.content_layer {
            layer.remove_displayed_image();
        }
    }

    fn keep_video_frame_until_window_server_lets_go(&mut self, frame: Rc<VideoFrameForWindowServer>) {
        self.replaced_video_frames.push(ReplacedVideoFrame {
            _frame: frame,
            replaced_at: Instant::now(),
        });
    }

    /// Lets go of the replaced frames that the window server can no longer be reading, and answers when to look again,
    /// where frames are left.
    fn release_video_frames_window_server_let_go_of(&mut self) -> Option<Duration> {
        if self.replaced_video_frames.is_empty() {
            return None;
        }
        // A replacement made while changes are held off screen reaches the window server only once they are released.
        if changes_are_held_off_screen() {
            return Some(TIME_BETWEEN_LOOKS_AT_REPLACED_FRAMES_WHILE_CHANGES_ARE_HELD);
        }
        let now = Instant::now();
        let last_released = changes_last_released_on_screen();
        let shown_on_screen_since = |replaced: &ReplacedVideoFrame| {
            last_released.map_or(replaced.replaced_at, |released| released.max(replaced.replaced_at))
        };
        self.replaced_video_frames.retain(|replaced| {
            now.duration_since(shown_on_screen_since(replaced)) < TIME_WINDOW_SERVER_MAY_READ_A_REPLACED_SURFACE
        });
        self.replaced_video_frames
            .iter()
            .map(|replaced| {
                (shown_on_screen_since(replaced) + TIME_WINDOW_SERVER_MAY_READ_A_REPLACED_SURFACE)
                    .saturating_duration_since(now)
            })
            .min()
    }
}

/// The mask stays in the space of the path, and its transform maps that space into the clip layer, whose points have
/// their origin at the origin of the clip rect: from the path's space to the frame's pixels, then to pixels from the
/// origin of the clip rect, then to points. With its anchor point and position at the origin, the mask is placed by
/// that transform alone.
fn update_path_clip_mask(native: &mut Sublayer, placement: &WindowServerPlacement, device_pixel_ratio: f64) {
    let clip_layer = &native.outermost_layer;
    let Some(path_clip) = &placement.path_clip else {
        if native.path_clip_mask.take().is_some() {
            clip_layer.set_mask(None);
        }
        return;
    };
    let path_identity = path_clip.path.identity();
    let even_odd = path_clip.fill_rule == WindingRule::EvenOdd;
    let (mask_layer, shown_path_identity, shown_even_odd) = native.path_clip_mask.get_or_insert_with(|| {
        let mask_layer = ShapeLayer::new();
        mask_layer.layer().set_position(CGPoint { x: 0.0, y: 0.0 });
        clip_layer.set_mask(Some(mask_layer.layer()));
        // The fill rule of a new shape layer is the non-zero one.
        (mask_layer, 0, false)
    });
    if *shown_path_identity != path_identity {
        let outline = outline_of_path(&path_clip.path);
        mask_layer.set_path(outline.as_ptr());
        *shown_path_identity = path_identity;
    }
    if *shown_even_odd != even_odd {
        mask_layer.set_fill_rule_even_odd(even_odd);
        *shown_even_odd = even_odd;
    }
    let path_to_clip_layer = to_core_graphics(path_clip.path_to_viewport)
        .then(CGAffineTransform::translation(
            -f64::from(placement.clip_rect.x),
            -f64::from(placement.clip_rect.y),
        ))
        .then(CGAffineTransform::scale(
            1.0 / device_pixel_ratio,
            1.0 / device_pixel_ratio,
        ));
    mask_layer.layer().set_affine_transform(path_to_clip_layer);
}
