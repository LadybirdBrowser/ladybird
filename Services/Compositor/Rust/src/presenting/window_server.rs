/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the window server shows of a frame by itself: the media layers it can show, the painted layers it shows tile by
//! tile, each in the groups that place and clip it, and the surfaces it shows the overlays of the compositor from. A
//! window server that does not show tiles shows a painted layer from one surface that the tiles are copied into.

use super::layers::{LayerCompositingInFrame, PaintedLayers};
use super::tiles::TileDraw;
use super::{ContextPresenting, FrameToPaint, PresentingEnvironment, rect_of_size, size_of};
use crate::raster_host::{
    FfiSublayer, FfiSublayerBackdrop, FfiSublayerGroup, FfiSublayerPlacement, LiveContent, MediaKind,
    SublayerContentKind,
};
use crate::surfaces::painting_surface::PaintingSurface;
use crate::surfaces::presentable_surface::{PresentableSurface, PresentableSurfacePool};
use libcompositing_rust::display_list::layers::{
    BandIndex, LayerContent, LayerIdentity, LayerIndex, SpaceToFrame, StackLayer, WindowServerCompositing,
    WindowServerPlacementsOfMedia,
};
use libcompositing_rust::display_list::summary::LiveContent as LiveMedia;
use libcompositing_rust::display_list::window_server_placement::{
    WhyWindowServerCannotShowMedia, WindowServerPlacement,
};
use libgfx_rust::{Color, CornerRadii, FloatRect, IntRect, IntSize, ScalingMode};
use std::collections::{HashMap, HashSet};
use std::hash::{Hash, Hasher};
use std::rc::Rc;

const CANVAS_SUBLAYER_IDENTITY_TAG: u64 = 1 << 60;
const VIDEO_SUBLAYER_IDENTITY_TAG: u64 = 2 << 60;
const PAINTED_LAYER_SUBLAYER_IDENTITY_TAG: u64 = 3 << 60;
const COMPOSITOR_OVERLAY_SUBLAYER_IDENTITY: u64 = 4 << 60;
const SUBLAYER_OF_TILED_LAYER_IDENTITY_TAG: u64 = 5 << 60;
const SUBLAYER_IDENTITY_BITS_BELOW_TAG: u64 = (1 << 60) - 1;

// The window server composites each sublayer on its own, so a frame with more of them than this is composited by the
// compositor instead. Pages show a few hundred tiles at most.
const MAXIMUM_SUBLAYERS_THE_WINDOW_SERVER_SHOWS_OF_FRAME: usize = 1024;

/// What a sublayer is of a painted layer the window server shows tile by tile.
#[derive(Hash)]
enum PartOfTiledLayer {
    GroupClippingToCompositeClip,
    GroupClippingToRoundedCompositeClip { ordinal: usize },
    GroupOfSpace,
    Tile { column: i32, row: i32 },
    FillOfJoinedOneColorTiles { ordinal: usize },
}

/// What a sublayer is of a band of a composited effect the window server composites.
#[derive(Hash)]
enum PartOfCompositedBand {
    GroupClippingToClipAroundEffect { ordinal: usize },
    GroupOfEffect,
    GroupClippingToRoundedBackdropRegion,
    Backdrop,
}

fn identity_of_sublayer_that_is(part: impl Hash, of_layer_or_composited_band: impl Hash) -> u64 {
    let mut hasher = std::collections::hash_map::DefaultHasher::new();
    of_layer_or_composited_band.hash(&mut hasher);
    part.hash(&mut hasher);
    SUBLAYER_OF_TILED_LAYER_IDENTITY_TAG | (hasher.finish() & SUBLAYER_IDENTITY_BITS_BELOW_TAG)
}

fn group(identity: u64, group_index: i32, group: FfiSublayerGroup) -> FfiSublayer {
    FfiSublayer {
        group_index,
        group,
        ..FfiSublayer::of_kind(identity, SublayerContentKind::Group)
    }
}

/// A group that clips its sublayers to a rect of the pixels it shares with the group it is in.
fn group_clipping_to(identity: u64, group_index: i32, clip_rect: FloatRect, corner_radii: CornerRadii) -> FfiSublayer {
    group(
        identity,
        group_index,
        FfiSublayerGroup {
            clips: true,
            clip_rect,
            corner_radii,
            ..FfiSublayerGroup::default()
        },
    )
}

// The checks of a plan keep the ordinal of a media layer below the most media layers a plan makes, so it stays clear of
// the tag.
const MEDIA_ORDINAL_SHIFT_PAST_CANVAS_AND_VIDEO_IDS: u32 = 44;

struct PaintedLayerPresentation {
    sublayer_ordinal: u32,
    surface_pool: PresentableSurfacePool,
    latest_surface: Option<Rc<PresentableSurface>>,
    rect_in_frame: IntRect,
    copied_content_version: u64,
    // Under a composite clip, the same rect of the frame shows other tiles once the layer's space scrolls.
    copied_with_compositing: Option<LayerCompositingInFrame>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum WhyMediaIsReplayed {
    ContextIsNotShownInARemoteLayer,
    NoGpu,
    ContextIsPresentedFromCpuMemory,
    Placement(WhyWindowServerCannotShowMedia),
    NoPlacement(LiveMedia),
    CanvasHasNoPresentableSurface,
    VideoHasNoFrame,
    HighDynamicRangeIsNotShownNow,
    WindowServerCannotShowThePlacement,
    TilesWerePaintedStraightIntoTheFrame,
    InsideAnEffectTheCompositorComposites,
    UnderALayerTheFrameHolds,
}

impl std::fmt::Display for WhyMediaIsReplayed {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(match self {
            WhyMediaIsReplayed::ContextIsNotShownInARemoteLayer => "the context is not shown in a remote layer",
            WhyMediaIsReplayed::NoGpu => "the compositor has no GPU",
            WhyMediaIsReplayed::ContextIsPresentedFromCpuMemory => "the context is presented from CPU memory",
            WhyMediaIsReplayed::Placement(rejection) => return write!(formatter, "{rejection}"),
            WhyMediaIsReplayed::NoPlacement(LiveMedia::ImageFrame(_)) => "the window server does not show images",
            WhyMediaIsReplayed::NoPlacement(LiveMedia::CompositedContext(_)) => {
                "the window server does not show nested contexts"
            }
            WhyMediaIsReplayed::NoPlacement(_) => "where the window server would show it is not worked out",
            WhyMediaIsReplayed::CanvasHasNoPresentableSurface => "the canvas has no surface the window server can show",
            WhyMediaIsReplayed::VideoHasNoFrame => "the video has no frame",
            WhyMediaIsReplayed::HighDynamicRangeIsNotShownNow => {
                "the window server does not show high dynamic range now"
            }
            WhyMediaIsReplayed::WindowServerCannotShowThePlacement => "the window server cannot show it where it is",
            WhyMediaIsReplayed::TilesWerePaintedStraightIntoTheFrame => "tiles were painted straight into the frame",
            WhyMediaIsReplayed::InsideAnEffectTheCompositorComposites => {
                "it is inside an effect the compositor composites"
            }
            WhyMediaIsReplayed::UnderALayerTheFrameHolds => "it is under a layer the frame holds",
        })
    }
}

#[derive(Default)]
pub(super) struct WindowServerLayers {
    presentation_by_layer: HashMap<LayerIdentity, PaintedLayerPresentation>,
    compositor_overlays_surface_pool: PresentableSurfacePool,
    latest_compositor_overlays_surface_and_rect: Option<(Rc<PresentableSurface>, IntRect)>,
    /// Whether the surface holds the overlays of the frame painted last, rather than those of a frame before it whose
    /// overlays went into the frame's store since.
    compositor_overlays_surface_holds_overlays_of_latest_frame: bool,
    next_sublayer_ordinal: u32,
    /// A surface on the GPU the compositor renders with, which tiles are made like for a frame painted into no store.
    surface_tiles_are_made_like_without_store: Option<Rc<PaintingSurface>>,
}

impl WindowServerLayers {
    pub(super) fn keep_only_layers(&mut self, identities_in_stack: &HashSet<LayerIdentity>) {
        self.presentation_by_layer
            .retain(|identity, _| identities_in_stack.contains(identity));
    }

    /// Tells that the overlays of a frame were painted into the frame's store, so that the surface the window server
    /// shows overlays from holds those of an earlier frame.
    pub(super) fn did_paint_compositor_overlays_into_store(&mut self) {
        self.compositor_overlays_surface_holds_overlays_of_latest_frame = false;
    }

    /// A surface that tiles are made like for a frame painted into no store, or none where it cannot be made.
    pub(super) fn surface_tiles_are_made_like_without_store(
        &mut self,
        env: &PresentingEnvironment<'_>,
    ) -> Option<Rc<PaintingSurface>> {
        if self.surface_tiles_are_made_like_without_store.is_none() {
            self.surface_tiles_are_made_like_without_store =
                env.raster.create_surface(IntSize { width: 1, height: 1 }, None, false);
        }
        self.surface_tiles_are_made_like_without_store.clone()
    }
}

/// The placement as the window server is handed it, whose path contours are lent from `placement`.
fn sublayer_placement_of(placement: &WindowServerPlacement) -> FfiSublayerPlacement {
    let rounded_clip = placement.rounded_clip;
    let path_clip = placement.path_clip.as_ref();
    FfiSublayerPlacement {
        content_rect: placement.content_rect,
        content_to_frame: placement.content_to_viewport.values,
        clip_rect: placement.clip_rect,
        has_rounded_clip: rounded_clip.is_some(),
        rounded_clip_rect: rounded_clip.map(|clip| clip.rect).unwrap_or_default(),
        rounded_clip_radius: rounded_clip.map_or(0.0, |clip| clip.radius),
        rounds_top_left: rounded_clip.is_some_and(|clip| clip.rounded_corners.top_left),
        rounds_top_right: rounded_clip.is_some_and(|clip| clip.rounded_corners.top_right),
        rounds_bottom_right: rounded_clip.is_some_and(|clip| clip.rounded_corners.bottom_right),
        rounds_bottom_left: rounded_clip.is_some_and(|clip| clip.rounded_corners.bottom_left),
        is_clipped_by_path: path_clip.is_some(),
        clip_path_identity: path_clip.map_or(0, |clip| clip.path.identity()),
        clip_path_fill_rule: path_clip.map(|clip| clip.fill_rule).unwrap_or_default(),
        clip_path_to_frame: path_clip.map_or([0.0; 6], |clip| clip.path_to_viewport.values),
        clip_path_contours: path_clip.map(|clip| clip.path.geometry_data()).unwrap_or_default(),
        scaling_mode: placement.scaling_mode,
    }
}

fn unclipped_untransformed_surface_sublayer(
    identity: u64,
    surface: &Rc<PresentableSurface>,
    rect_in_frame: IntRect,
) -> FfiSublayer {
    FfiSublayer {
        presentable_surface: surface.as_ffi(),
        placement: FfiSublayerPlacement {
            content_rect: rect_in_frame,
            content_to_frame: [1.0, 0.0, 0.0, 1.0, 0.0, 0.0],
            clip_rect: rect_in_frame.to_float(),
            scaling_mode: ScalingMode::NearestNeighbor,
            ..Default::default()
        },
        ..FfiSublayer::of_kind(identity, SublayerContentKind::PresentableSurface)
    }
}

impl ContextPresenting {
    pub(super) fn why_window_server_shows_no_media_of_this_context(
        &self,
        env: &PresentingEnvironment<'_>,
    ) -> Option<WhyMediaIsReplayed> {
        if !self.shown_in_remote_layer {
            return Some(WhyMediaIsReplayed::ContextIsNotShownInARemoteLayer);
        }
        if !env.has_gpu {
            return Some(WhyMediaIsReplayed::NoGpu);
        }
        if self.presents_from_cpu_memory {
            return Some(WhyMediaIsReplayed::ContextIsPresentedFromCpuMemory);
        }
        None
    }

    /// What the window server composites by itself of the layers of this context's frames, besides canvases and
    /// videos. It shows no tiles of a frame it scales to fit a view, as it would filter each tile apart from the tiles
    /// beside it.
    pub(super) fn window_server_compositing(&self, env: &PresentingEnvironment<'_>) -> WindowServerCompositing {
        let compositing_of_window_server = env
            .window_server_showing_context()
            .map(|window_server| env.raster.window_server_compositing(window_server))
            .unwrap_or_default();
        let shows_tiles_of_painted_layers = self.why_window_server_shows_no_media_of_this_context(env).is_none()
            && !self.scales_frames_to_fit
            && compositing_of_window_server.shows_tiles_of_painted_layers;
        WindowServerCompositing {
            shows_tiles_of_painted_layers,
            filters_what_is_below_layers: shows_tiles_of_painted_layers
                && compositing_of_window_server.filters_what_is_below_layers,
        }
    }

    pub(super) fn why_window_server_does_not_show_media_layer(
        &self,
        env: &PresentingEnvironment<'_>,
        window_server_placements_of_media: &WindowServerPlacementsOfMedia,
        layer_index: LayerIndex,
        layer: &StackLayer,
        live_content: &LiveContent,
    ) -> Option<WhyMediaIsReplayed> {
        let LayerIdentity::Media { content, .. } = &layer.identity else {
            return None;
        };
        let placement = match window_server_placements_of_media.of_layer(layer_index) {
            Some(Ok(placement)) => placement,
            Some(Err(why)) => return Some(WhyMediaIsReplayed::Placement(*why)),
            None => return Some(WhyMediaIsReplayed::NoPlacement(*content)),
        };
        let (kind, id) = match content {
            LiveMedia::Canvas(id) => {
                let has_presentable_surface = live_content
                    .canvases
                    .get(&id.0)
                    .is_some_and(|canvas| canvas.has_presentable_surface);
                if !has_presentable_surface {
                    return Some(WhyMediaIsReplayed::CanvasHasNoPresentableSurface);
                }
                (MediaKind::Canvas, id.0)
            }
            LiveMedia::VideoFrame(id) => {
                let Some(sink) = live_content.video_sinks.get(&id.0).filter(|sink| sink.has_frame) else {
                    return Some(WhyMediaIsReplayed::VideoHasNoFrame);
                };
                if !env.window_server_may_show_high_dynamic_range && sink.frame_holds_high_dynamic_range {
                    return Some(WhyMediaIsReplayed::HighDynamicRangeIsNotShownNow);
                }
                (MediaKind::Video, id.0)
            }
            LiveMedia::ImageFrame(_) | LiveMedia::CompositedContext(_) => {
                return Some(WhyMediaIsReplayed::NoPlacement(*content));
            }
        };
        let Some(remote_layer) = &env.remote_layer else {
            return Some(WhyMediaIsReplayed::ContextIsNotShownInARemoteLayer);
        };
        if !env.raster.remote_layer_can_show(
            &remote_layer.layer,
            env.context_id,
            kind,
            id,
            &sublayer_placement_of(placement),
            self.device_pixel_ratio,
        ) {
            return Some(WhyMediaIsReplayed::WindowServerCannotShowThePlacement);
        }
        None
    }

    pub(super) fn present_painted_layers_to_window_server(
        &mut self,
        env: &PresentingEnvironment<'_>,
        painted_layers: &PaintedLayers,
        like: &Rc<PaintingSurface>,
    ) -> bool {
        let raster = env.raster;
        let viewport_rect = rect_of_size(self.viewport_size);
        let layers = &mut self.layers;
        let (stack, compositing_of_layers) = (&painted_layers.stack, &painted_layers.compositing_of_layers);
        for compositing in compositing_of_layers
            .iter()
            .skip(1)
            .filter(|compositing| compositing.shown_by_window_server)
            .filter(|compositing| matches!(stack.layers[compositing.layer_index.0].content, LayerContent::Painted))
        {
            let Some(content_version_of_held_tiles) = layers.tiles.content_version_of(&compositing.identity) else {
                continue;
            };
            let held_rect_in_frame =
                compositing.rect_in_frame(layers.tiles.bounds_held_by(&compositing.identity), viewport_rect);
            if held_rect_in_frame.is_empty() {
                if let Some(presentation) = layers
                    .window_server
                    .presentation_by_layer
                    .get_mut(&compositing.identity)
                {
                    presentation.latest_surface = None;
                }
                continue;
            }
            let presentation = layers
                .window_server
                .presentation_by_layer
                .entry(compositing.identity)
                .or_insert_with(|| {
                    let sublayer_ordinal = layers.window_server.next_sublayer_ordinal;
                    layers.window_server.next_sublayer_ordinal += 1;
                    PaintedLayerPresentation {
                        sublayer_ordinal,
                        surface_pool: PresentableSurfacePool::default(),
                        latest_surface: None,
                        rect_in_frame: IntRect::default(),
                        copied_content_version: 0,
                        copied_with_compositing: None,
                    }
                });
            if presentation.latest_surface.is_some()
                && presentation.rect_in_frame == held_rect_in_frame
                && presentation.copied_content_version == content_version_of_held_tiles
                && presentation.copied_with_compositing.as_ref() == Some(compositing)
            {
                continue;
            }
            let held_size = size_of(held_rect_in_frame);
            let Some(presentable) =
                presentation
                    .surface_pool
                    .surface_for_next_content(held_size, like.gpu_backend_context(), || {
                        raster.create_presentable_surface(held_size, like)
                    })
            else {
                return false;
            };
            let target = presentable.painting_surface();
            raster.clear(target, Color::TRANSPARENT);
            let save_count = raster.save(target);
            raster.translate(target, -held_rect_in_frame.x as f32, -held_rect_in_frame.y as f32);
            layers.tiles.copy_into(raster, target, held_rect_in_frame, compositing);
            raster.restore_to_count(target, save_count);
            presentation.latest_surface = Some(presentable);
            presentation.rect_in_frame = held_rect_in_frame;
            presentation.copied_content_version = content_version_of_held_tiles;
            presentation.copied_with_compositing = Some(compositing.clone());
        }
        true
    }

    /// Paints the overlays of the frame into a surface the window server shows them from, unless the surface holds
    /// them already: it holds those of the frame painted last, which `overlays_draw_same_as_in_latest_frame` says draw
    /// the same. Answers false where no surface could be had.
    pub(super) fn present_compositor_overlays_to_window_server(
        &mut self,
        env: &PresentingEnvironment<'_>,
        frame: &FrameToPaint<'_>,
        like: &Rc<PaintingSurface>,
        overlays_draw_same_as_in_latest_frame: bool,
    ) -> bool {
        let raster = env.raster;
        let mut overlays_rect = frame.frame_inputs.overlays.bounds;
        let draws_overlay_of_layers = self.layer_overlay.latest_frame().is_some();
        if draws_overlay_of_layers {
            overlays_rect = overlays_rect.united(self.raster_rect());
        }
        if overlays_rect.is_empty() {
            self.layers.window_server.latest_compositor_overlays_surface_and_rect = None;
            self.layers
                .window_server
                .compositor_overlays_surface_holds_overlays_of_latest_frame = true;
            return true;
        }
        let surface_holds_overlays_of_frame = overlays_draw_same_as_in_latest_frame
            && !draws_overlay_of_layers
            && self
                .layers
                .window_server
                .compositor_overlays_surface_holds_overlays_of_latest_frame
            && self
                .layers
                .window_server
                .latest_compositor_overlays_surface_and_rect
                .as_ref()
                .is_some_and(|(_, rect)| *rect == overlays_rect);
        if surface_holds_overlays_of_frame {
            return true;
        }
        let overlays_size = size_of(overlays_rect);
        let Some(presentable) = self
            .layers
            .window_server
            .compositor_overlays_surface_pool
            .surface_for_next_content(overlays_size, like.gpu_backend_context(), || {
                raster.create_presentable_surface(overlays_size, like)
            })
        else {
            return false;
        };
        let target = presentable.painting_surface();
        raster.clear(target, Color::TRANSPARENT);
        let save_count = raster.save(target);
        raster.translate(target, -overlays_rect.x as f32, -overlays_rect.y as f32);
        self.paint_overlays(env, target, frame.frame_inputs, true);
        raster.restore_to_count(target, save_count);
        self.layers.window_server.latest_compositor_overlays_surface_and_rect = Some((presentable, overlays_rect));
        // An overlay of the layers shows what the frame did, which the next frame does not repeat.
        self.layers
            .window_server
            .compositor_overlays_surface_holds_overlays_of_latest_frame = !draws_overlay_of_layers;
        true
    }

    /// Presents to the window server what it shows of the frame besides canvases and videos, once the tiles of the
    /// frame are rastered, and answers the sublayers it then shows, bottom to top. None where a surface could not be
    /// had, or where the frame has more sublayers than the window server shows of one.
    pub(super) fn present_layers_to_window_server(
        &mut self,
        env: &PresentingEnvironment<'_>,
        frame: &FrameToPaint<'_>,
        painted_layers: &PaintedLayers,
        like: &Rc<PaintingSurface>,
        live_content: &LiveContent,
        overlays_draw_same_as_in_latest_frame: bool,
    ) -> Option<Vec<FfiSublayer>> {
        if !painted_layers.window_server_compositing.shows_tiles_of_painted_layers
            && !self.present_painted_layers_to_window_server(env, painted_layers, like)
        {
            return None;
        }
        if !self.present_compositor_overlays_to_window_server(env, frame, like, overlays_draw_same_as_in_latest_frame) {
            return None;
        }
        let sublayers = self.sublayers_shown_by_window_server(painted_layers, live_content);
        (sublayers.len() <= MAXIMUM_SUBLAYERS_THE_WINDOW_SERVER_SHOWS_OF_FRAME).then_some(sublayers)
    }

    /// Appends the sublayers of a painted layer the window server shows tile by tile, in the group of `group_index`,
    /// whose pixels are the frame's: a group per clip the layer is composited within, each in the one before, and in
    /// the innermost, the group of the layer's space, which holds the tiles of the layer that reach into the viewport,
    /// and a fill per run of tiles of one color.
    fn append_sublayers_of_tiled_layer(
        &self,
        sublayers: &mut Vec<FfiSublayer>,
        compositing: &LayerCompositingInFrame,
        viewport_rect: IntRect,
        group_index: i32,
    ) {
        let draws = self.layers.tiles.draws_reaching_into(compositing, viewport_rect);
        if draws.is_empty() {
            return;
        }
        let identity_of = |part: PartOfTiledLayer| identity_of_sublayer_that_is(part, compositing.identity);
        let mut innermost_group_index = group_index;
        if let Some(composite_clip) = compositing.composite_clip {
            sublayers.push(group_clipping_to(
                identity_of(PartOfTiledLayer::GroupClippingToCompositeClip),
                innermost_group_index,
                composite_clip.to_float(),
                CornerRadii::default(),
            ));
            innermost_group_index = sublayers.len() as i32 - 1;
        }
        for (ordinal, (rounded_clip_rect, corner_radii)) in compositing.rounded_composite_clips.iter().enumerate() {
            sublayers.push(group_clipping_to(
                identity_of(PartOfTiledLayer::GroupClippingToRoundedCompositeClip { ordinal }),
                innermost_group_index,
                rounded_clip_rect.to_float(),
                *corner_radii,
            ));
            innermost_group_index = sublayers.len() as i32 - 1;
        }
        sublayers.push(group(
            identity_of(PartOfTiledLayer::GroupOfSpace),
            innermost_group_index,
            FfiSublayerGroup {
                to_parent: match compositing.space_to_frame {
                    SpaceToFrame::Offset(offset) => [1.0, 0.0, 0.0, 1.0, offset.x as f32, offset.y as f32],
                    SpaceToFrame::Transform(transform) => transform.values,
                },
                ..FfiSublayerGroup::default()
            },
        ));
        let index_of_group_of_space = sublayers.len() as i32 - 1;
        let mut ordinal_of_fill = 0;
        for draw in draws {
            match draw {
                TileDraw::TileSurface {
                    coord,
                    surface,
                    every_pixel_is_opaque,
                    rect_in_layer_space,
                } => sublayers.push(FfiSublayer {
                    group_index: index_of_group_of_space,
                    presentable_surface: surface.as_ffi(),
                    rect: rect_in_layer_space,
                    every_pixel_is_opaque,
                    ..FfiSublayer::of_kind(
                        identity_of(PartOfTiledLayer::Tile {
                            column: coord.column,
                            row: coord.row,
                        }),
                        SublayerContentKind::Tile,
                    )
                }),
                TileDraw::FillOfJoinedOneColorTiles {
                    rect_in_layer_space,
                    color,
                } => {
                    if color.alpha() == 0 {
                        continue;
                    }
                    sublayers.push(FfiSublayer {
                        group_index: index_of_group_of_space,
                        rect: rect_in_layer_space,
                        color,
                        ..FfiSublayer::of_kind(
                            identity_of(PartOfTiledLayer::FillOfJoinedOneColorTiles {
                                ordinal: ordinal_of_fill,
                            }),
                            SublayerContentKind::Color,
                        )
                    });
                    ordinal_of_fill += 1;
                }
            }
        }
    }

    /// Appends the groups of a band of a composited effect the window server composites, in the group of
    /// `group_index`, whose pixels are the frame's: a group per clip the effect is entered within, each in the one
    /// before, and in the innermost, the group of the effect, which composites the layers of the band at the effect's
    /// opacity. Answers the index of the group of the effect, whose pixels are the frame's too.
    fn append_groups_of_composited_band(
        sublayers: &mut Vec<FfiSublayer>,
        painted: &PaintedLayers,
        band_index: BandIndex,
        group_index: i32,
    ) -> i32 {
        let band = &painted.stack.bands[band_index.0];
        let node_index = band.placement_node_index;
        // The frame holds the layers inside an effect the window server cannot composite.
        let Some(Ok(placement)) = painted.window_server_placements_of_effects.of_node(node_index) else {
            return group_index;
        };
        let composited_band = (painted.stack.nodes[node_index.0].identity, band.band_ordinal_in_node);
        let identity_of = |part: PartOfCompositedBand| identity_of_sublayer_that_is(part, composited_band);
        let mut innermost_group_index = group_index;
        for (ordinal, (clip_rect, corner_radii)) in placement.clips.iter().enumerate() {
            sublayers.push(group_clipping_to(
                identity_of(PartOfCompositedBand::GroupClippingToClipAroundEffect { ordinal }),
                innermost_group_index,
                *clip_rect,
                *corner_radii,
            ));
            innermost_group_index = sublayers.len() as i32 - 1;
        }
        sublayers.push(group(
            identity_of(PartOfCompositedBand::GroupOfEffect),
            innermost_group_index,
            FfiSublayerGroup {
                opacity: placement.opacity.clamp(0.0, 1.0),
                ..FfiSublayerGroup::default()
            },
        ));
        let index_of_group_of_effect = sublayers.len() as i32 - 1;
        // The filtered backdrop is below all the effect holds.
        if let Some(backdrop) = &placement.backdrop {
            let mut index_of_group_of_backdrop = index_of_group_of_effect;
            if backdrop.corner_radii.has_any_radius() {
                sublayers.push(group_clipping_to(
                    identity_of(PartOfCompositedBand::GroupClippingToRoundedBackdropRegion),
                    index_of_group_of_effect,
                    backdrop.region,
                    backdrop.corner_radii,
                ));
                index_of_group_of_backdrop = sublayers.len() as i32 - 1;
            }
            let filters = &backdrop.filters;
            sublayers.push(FfiSublayer {
                group_index: index_of_group_of_backdrop,
                backdrop: FfiSublayerBackdrop {
                    region: backdrop.region,
                    has_color_matrix_before_blur: filters.color_matrix_before_blur.is_some(),
                    color_matrix_before_blur: filters.color_matrix_before_blur.unwrap_or_default(),
                    has_blur: filters.blur_standard_deviation.is_some(),
                    blur_standard_deviation: filters.blur_standard_deviation.unwrap_or_default(),
                    has_color_matrix_after_blur: filters.color_matrix_after_blur.is_some(),
                    color_matrix_after_blur: filters.color_matrix_after_blur.unwrap_or_default(),
                },
                ..FfiSublayer::of_kind(
                    identity_of(PartOfCompositedBand::Backdrop),
                    SublayerContentKind::Backdrop,
                )
            });
        }
        index_of_group_of_effect
    }

    /// The sublayers the window server shows of a frame that did `painted` with its layers, bottom to top, each group
    /// before the sublayers in it.
    pub(super) fn sublayers_shown_by_window_server(
        &self,
        painted: &PaintedLayers,
        live_content: &LiveContent,
    ) -> Vec<FfiSublayer> {
        let layers = &self.layers.window_server;
        let mut sublayers = Vec::new();
        if !painted.shown_by_window_server() {
            return sublayers;
        }
        let viewport_rect = self.raster_rect();
        let mut entered_composited_bands_and_their_groups: Vec<(BandIndex, i32)> = Vec::new();
        let mut composited_bands_around_layer: Vec<BandIndex> = Vec::new();
        for compositing in painted
            .compositing_of_layers
            .iter()
            .filter(|compositing| compositing.shown_by_window_server)
            .filter(|compositing| !self.layer_overlay.hides_layer(&compositing.identity))
        {
            let layer_index = compositing.layer_index;
            let layer = compositing.layer_in(&painted.stack);
            composited_bands_around_layer.clear();
            composited_bands_around_layer
                .extend(painted.stack.composited_bands_around_layer_innermost_first(layer_index));
            composited_bands_around_layer.reverse();
            let count_of_bands_still_entered = entered_composited_bands_and_their_groups
                .iter()
                .zip(&composited_bands_around_layer)
                .take_while(|((entered_band, _), band_around_layer)| entered_band == *band_around_layer)
                .count();
            entered_composited_bands_and_their_groups.truncate(count_of_bands_still_entered);
            for &band_index in &composited_bands_around_layer[count_of_bands_still_entered..] {
                let index_of_group_around = entered_composited_bands_and_their_groups
                    .last()
                    .map_or(-1, |(_, group_index)| *group_index);
                let index_of_group_of_effect =
                    Self::append_groups_of_composited_band(&mut sublayers, painted, band_index, index_of_group_around);
                entered_composited_bands_and_their_groups.push((band_index, index_of_group_of_effect));
            }
            let group_index = entered_composited_bands_and_their_groups
                .last()
                .map_or(-1, |(_, group_index)| *group_index);
            if let LayerIdentity::Media { content, ordinal } = layer.identity {
                let Some(Ok(placement)) = painted.window_server_placements_of_media.of_layer(layer_index) else {
                    continue;
                };
                let (kind, id, sublayer_identity_of_media, has_content) = match content {
                    LiveMedia::Canvas(id) => (
                        SublayerContentKind::Canvas,
                        id.0,
                        CANVAS_SUBLAYER_IDENTITY_TAG | id.0,
                        live_content
                            .canvases
                            .get(&id.0)
                            .is_some_and(|canvas| canvas.has_presentable_surface),
                    ),
                    LiveMedia::VideoFrame(id) => (
                        SublayerContentKind::Video,
                        id.0,
                        VIDEO_SUBLAYER_IDENTITY_TAG | id.0,
                        live_content
                            .video_sinks
                            .get(&id.0)
                            .is_some_and(|sink| sink.has_frame && sink.frame_has_surface),
                    ),
                    LiveMedia::ImageFrame(_) | LiveMedia::CompositedContext(_) => continue,
                };
                if !has_content {
                    continue;
                }
                sublayers.push(FfiSublayer {
                    group_index,
                    media_id: id,
                    placement: sublayer_placement_of(placement),
                    ..FfiSublayer::of_kind(
                        sublayer_identity_of_media
                            | (u64::from(ordinal) << MEDIA_ORDINAL_SHIFT_PAST_CANVAS_AND_VIDEO_IDS),
                        kind,
                    )
                });
                continue;
            }
            if painted.window_server_compositing.shows_tiles_of_painted_layers {
                self.append_sublayers_of_tiled_layer(&mut sublayers, compositing, viewport_rect, group_index);
                continue;
            }
            let Some(presentation) = layers.presentation_by_layer.get(&compositing.identity) else {
                continue;
            };
            let Some(surface) = &presentation.latest_surface else {
                continue;
            };
            sublayers.push(unclipped_untransformed_surface_sublayer(
                PAINTED_LAYER_SUBLAYER_IDENTITY_TAG | u64::from(presentation.sublayer_ordinal),
                surface,
                presentation.rect_in_frame,
            ));
        }
        if let Some((surface, rect_in_frame)) = &layers.latest_compositor_overlays_surface_and_rect {
            sublayers.push(unclipped_untransformed_surface_sublayer(
                COMPOSITOR_OVERLAY_SUBLAYER_IDENTITY,
                surface,
                *rect_in_frame,
            ));
        }
        sublayers
    }
}
