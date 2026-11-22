/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the window server shows of a frame by itself: the media layers it can show, the painted layers it shows tile by
//! tile, each in the groups that place and clip it, and the surfaces it shows the overlays of the compositor from.

use super::layers::{CompositionStep, LayerCompositingInFrame, PaintedLayers, walk_composition};
use super::tiles::TileDraw;
use super::{
    ContextPresenting, FrameToPaint, PresentingEnvironment, rect_of_raster_shown_through, rect_of_size, size_of,
};
use crate::raster_host::LiveContent;
use crate::surfaces::painting_surface::PaintingSurface;
use crate::surfaces::presentable_surface::{PresentableSurface, PresentableSurfacePool};
use crate::window_server::{Backdrop, Group, MediaToShow, PlannedContent, PlannedSublayer, SublayerContent};
use libcompositing_rust::display_list::layers::{
    BandIndex, LayerContent, LayerIdentity, LayerIndex, SpaceToFrame, StackLayer, WindowServerCompositing,
    WindowServerPlacementsOfMedia,
};
use libcompositing_rust::display_list::summary::LiveContent as LiveMedia;
use libcompositing_rust::display_list::tiles::TileCoord;
use libcompositing_rust::display_list::window_server_placement::{
    WhyWindowServerCannotShowMedia, WindowServerPlacement,
};
use libgfx_rust::{AffineTransform, Color, CornerRadii, FloatRect, IntRect, ScalingMode};
use std::hash::{Hash, Hasher};
use std::rc::Rc;

const CANVAS_SUBLAYER_IDENTITY_TAG: u64 = 1 << 60;
const VIDEO_SUBLAYER_IDENTITY_TAG: u64 = 2 << 60;
const COMPOSITOR_OVERLAY_SUBLAYER_IDENTITY: u64 = 4 << 60;
const SUBLAYER_OF_TILED_LAYER_IDENTITY_TAG: u64 = 5 << 60;
const GROUP_OF_RASTER_SUBLAYER_IDENTITY: u64 = 6 << 60;
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

fn group(identity: u64, group_index: Option<usize>, group: Group) -> PlannedSublayer {
    PlannedSublayer {
        identity,
        group_index,
        content: SublayerContent::Group(group),
    }
}

/// A group that clips its sublayers to a rect of the pixels it shares with the group it is in.
fn group_clipping_to(
    identity: u64,
    group_index: Option<usize>,
    clip_rect: FloatRect,
    corner_radii: CornerRadii,
) -> PlannedSublayer {
    group(
        identity,
        group_index,
        Group {
            clip_rect: Some(clip_rect),
            corner_radii,
            ..Group::default()
        },
    )
}

// The checks of a plan keep the ordinal of a media layer below the most media layers a plan makes, so it stays clear of
// the tag.
const MEDIA_ORDINAL_SHIFT_PAST_CANVAS_AND_VIDEO_IDS: u32 = 44;

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
    WindowServerShowsNoTiles,
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
            WhyMediaIsReplayed::WindowServerShowsNoTiles => "the window server shows no tiles",
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
    compositor_overlays_surface_pool: PresentableSurfacePool,
    latest_compositor_overlays_surface_and_rect: Option<(Rc<PresentableSurface>, IntRect)>,
    /// Whether the surface holds the overlays of the frame painted last, rather than those of a frame before it whose
    /// overlays went into the frame's store since.
    compositor_overlays_surface_holds_overlays_of_latest_frame: bool,
}

impl WindowServerLayers {
    /// Tells that the overlays of a frame were painted into the frame's store, so that the surface the window server
    /// shows overlays from holds those of an earlier frame.
    pub(super) fn did_paint_compositor_overlays_into_store(&mut self) {
        self.compositor_overlays_surface_holds_overlays_of_latest_frame = false;
    }
}

fn unclipped_untransformed_surface_sublayer(
    identity: u64,
    surface: &Rc<PresentableSurface>,
    rect_in_frame: IntRect,
) -> PlannedSublayer {
    PlannedSublayer {
        identity,
        group_index: None,
        content: SublayerContent::Placed {
            content: PlannedContent::Surface(surface.clone()),
            placement: WindowServerPlacement {
                content_rect: rect_in_frame,
                content_to_viewport: AffineTransform::identity(),
                clip_rect: rect_in_frame.to_float(),
                rounded_clip: None,
                path_clip: None,
                scaling_mode: ScalingMode::NearestNeighbor,
            },
        },
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
        let Some(window_server) = env.window_server_showing_context() else {
            return Some(WhyMediaIsReplayed::ContextIsNotShownInARemoteLayer);
        };
        if !window_server.compositing().shows_tiles_of_painted_layers {
            return Some(WhyMediaIsReplayed::WindowServerShowsNoTiles);
        }
        None
    }

    /// What the window server composites by itself of the layers of this context's frames. It shows tiles and media
    /// of a context it shows anything of, and none of any other.
    pub(super) fn window_server_compositing(&self, env: &PresentingEnvironment<'_>) -> WindowServerCompositing {
        let compositing_of_window_server = env
            .window_server_showing_context()
            .map(|window_server| window_server.compositing())
            .unwrap_or_default();
        let shows_tiles_of_painted_layers = self.why_window_server_shows_no_media_of_this_context(env).is_none();
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
        let media = match content {
            LiveMedia::Canvas(id) => {
                let Some(surface) = live_content
                    .canvases
                    .get(&id.0)
                    .and_then(|canvas| canvas.presentable_surface.as_ref())
                else {
                    return Some(WhyMediaIsReplayed::CanvasHasNoPresentableSurface);
                };
                MediaToShow::Surface(surface)
            }
            LiveMedia::VideoFrame(id) => {
                let Some(frame) = live_content.video_sinks.get(&id.0).and_then(|sink| sink.frame.as_ref()) else {
                    return Some(WhyMediaIsReplayed::VideoHasNoFrame);
                };
                if !env.window_server_may_show_high_dynamic_range && frame.color.holds_high_dynamic_range() {
                    return Some(WhyMediaIsReplayed::HighDynamicRangeIsNotShownNow);
                }
                MediaToShow::Video(frame)
            }
            LiveMedia::ImageFrame(_)
            | LiveMedia::CompositedContext(_)
            | LiveMedia::Caret
            | LiveMedia::ScrollBar { .. } => {
                return Some(WhyMediaIsReplayed::NoPlacement(*content));
            }
        };
        let Some(remote_layer) = &env.remote_layer else {
            return Some(WhyMediaIsReplayed::ContextIsNotShownInARemoteLayer);
        };
        if !remote_layer.layer.can_show(placement, media) {
            return Some(WhyMediaIsReplayed::WindowServerCannotShowThePlacement);
        }
        None
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
        self.paint_overlays(env, target, frame.frame_inputs);
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
    ) -> Option<Vec<PlannedSublayer>> {
        if !self.present_compositor_overlays_to_window_server(env, frame, like, overlays_draw_same_as_in_latest_frame) {
            return None;
        }
        let sublayers = self.sublayers_shown_by_window_server(painted_layers, live_content);
        (sublayers.len() <= MAXIMUM_SUBLAYERS_THE_WINDOW_SERVER_SHOWS_OF_FRAME).then_some(sublayers)
    }

    /// Appends the sublayers of a painted layer the window server shows tile by tile, in the group of `group_index`,
    /// whose pixels are the frame's: a group per clip the layer is composited within, each in the one before, and in
    /// the innermost, the group of the layer's space, which holds `draws` of the layer: its tiles and a fill per run of
    /// tiles of one color.
    fn append_sublayers_of_tiled_layer(
        &self,
        sublayers: &mut Vec<PlannedSublayer>,
        compositing: &LayerCompositingInFrame,
        draws: Vec<TileDraw<'_>>,
        group_index: Option<usize>,
        raster_moves_by_whole_pixels_only: bool,
    ) {
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
            innermost_group_index = Some(sublayers.len() - 1);
        }
        for (ordinal, (rounded_clip_rect, corner_radii)) in compositing.rounded_composite_clips.iter().enumerate() {
            sublayers.push(group_clipping_to(
                identity_of(PartOfTiledLayer::GroupClippingToRoundedCompositeClip { ordinal }),
                innermost_group_index,
                rounded_clip_rect.to_float(),
                *corner_radii,
            ));
            innermost_group_index = Some(sublayers.len() - 1);
        }
        sublayers.push(group(
            identity_of(PartOfTiledLayer::GroupOfSpace),
            innermost_group_index,
            Group {
                to_parent: match compositing.space_to_frame {
                    SpaceToFrame::Offset(offset) => AffineTransform {
                        values: [1.0, 0.0, 0.0, 1.0, offset.x as f32, offset.y as f32],
                    },
                    SpaceToFrame::Transform(transform) => transform,
                },
                ..Group::default()
            },
        ));
        let index_of_group_of_space = Some(sublayers.len() - 1);
        // Scaled or moved by a fraction of a pixel, a tile is sampled at its border too, which can be transparent
        // beside an opaque tile.
        let window_server_samples_no_tile_border = raster_moves_by_whole_pixels_only
            && match compositing.space_to_frame {
                SpaceToFrame::Offset(_) => true,
                SpaceToFrame::Transform(transform) => AffineTransform::is_whole_pixel_translation(transform),
            };
        let mut ordinal_of_fill = 0;
        for draw in draws {
            match draw {
                TileDraw::TileSurface {
                    coord,
                    surface,
                    every_pixel_is_opaque,
                    rect_in_layer_space,
                    rect_in_surface,
                } => sublayers.push(PlannedSublayer {
                    identity: identity_of(PartOfTiledLayer::Tile {
                        column: coord.column,
                        row: coord.row,
                    }),
                    group_index: index_of_group_of_space,
                    content: SublayerContent::Tile {
                        surface: surface.clone(),
                        rect: rect_in_layer_space,
                        rect_in_surface,
                        every_pixel_is_opaque: every_pixel_is_opaque && window_server_samples_no_tile_border,
                    },
                }),
                TileDraw::FillOfJoinedOneColorTiles {
                    rect_in_layer_space,
                    color,
                } => {
                    if color.alpha() == 0 {
                        continue;
                    }
                    sublayers.push(PlannedSublayer {
                        identity: identity_of(PartOfTiledLayer::FillOfJoinedOneColorTiles {
                            ordinal: ordinal_of_fill,
                        }),
                        group_index: index_of_group_of_space,
                        content: SublayerContent::Color {
                            color,
                            rect: rect_in_layer_space,
                        },
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
        sublayers: &mut Vec<PlannedSublayer>,
        painted: &PaintedLayers,
        band_index: BandIndex,
        group_index: Option<usize>,
    ) -> Option<usize> {
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
            innermost_group_index = Some(sublayers.len() - 1);
        }
        sublayers.push(group(
            identity_of(PartOfCompositedBand::GroupOfEffect),
            innermost_group_index,
            Group {
                opacity: placement.opacity.clamp(0.0, 1.0),
                ..Group::default()
            },
        ));
        let index_of_group_of_effect = Some(sublayers.len() - 1);
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
                index_of_group_of_backdrop = Some(sublayers.len() - 1);
            }
            sublayers.push(PlannedSublayer {
                identity: identity_of(PartOfCompositedBand::Backdrop),
                group_index: index_of_group_of_backdrop,
                content: SublayerContent::Backdrop(Backdrop {
                    region: backdrop.region,
                    filters: backdrop.filters,
                }),
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
    ) -> Vec<PlannedSublayer> {
        let layers = &self.layers.window_server;
        let mut sublayers = Vec::new();
        if !painted.shown_by_window_server() {
            return sublayers;
        }
        // Every sublayer of the page is in the pixels of the raster, which this group maps to those of the frame. It
        // is there for frames that do not move the raster too, so that a frame that does moves no sublayer to another
        // group.
        sublayers.push(group(
            GROUP_OF_RASTER_SUBLAYER_IDENTITY,
            None,
            Group {
                to_parent: painted.raster_to_frame.unwrap_or_else(AffineTransform::identity),
                ..Group::default()
            },
        ));
        let index_of_group_of_raster = Some(sublayers.len() - 1);
        // The remote layer scales the sublayers of a frame it scales to fit a view along with the frame.
        let raster_moves_by_whole_pixels_only = !self.scales_frames_to_fit
            && painted
                .raster_to_frame
                .is_none_or(AffineTransform::is_whole_pixel_translation);
        let rect_of_raster_shown = rect_of_raster_shown_through(painted.raster_to_frame, self.raster_rect());
        let mut groups_of_entered_composited_bands: Vec<Option<usize>> = Vec::new();
        let layers_shown = painted
            .compositing_of_layers
            .iter()
            .filter(|compositing| compositing.shown_by_window_server)
            .filter(|compositing| !self.layer_overlay.hides_layer(&compositing.identity));
        let innermost_group_index = |groups_of_entered_composited_bands: &[Option<usize>]| {
            groups_of_entered_composited_bands
                .last()
                .copied()
                .unwrap_or(index_of_group_of_raster)
        };
        walk_composition(&painted.stack, layers_shown, |step| {
            let compositing = match step {
                CompositionStep::EnterBandOfCompositedEffect(band_index) => {
                    let index_of_group_around = innermost_group_index(&groups_of_entered_composited_bands);
                    groups_of_entered_composited_bands.push(Self::append_groups_of_composited_band(
                        &mut sublayers,
                        painted,
                        band_index,
                        index_of_group_around,
                    ));
                    return;
                }
                CompositionStep::LeaveInnermostBandEntered => {
                    groups_of_entered_composited_bands.pop();
                    return;
                }
                CompositionStep::Layer(compositing) => compositing,
            };
            let layer_index = compositing.layer_index;
            let layer = compositing.layer_in(&painted.stack);
            let group_index = innermost_group_index(&groups_of_entered_composited_bands);
            if let (LayerContent::Media { .. }, LayerIdentity::Media { content, ordinal }) =
                (&layer.content, layer.identity)
            {
                let Some(Ok(placement)) = painted.window_server_placements_of_media.of_layer(layer_index) else {
                    return;
                };
                let (planned_content, sublayer_identity_of_media, has_content) = match content {
                    LiveMedia::Canvas(id) => (
                        PlannedContent::Canvas(id.0),
                        CANVAS_SUBLAYER_IDENTITY_TAG | id.0,
                        live_content
                            .canvases
                            .get(&id.0)
                            .is_some_and(|canvas| canvas.presentable_surface.is_some()),
                    ),
                    LiveMedia::VideoFrame(id) => (
                        PlannedContent::Video(id.0),
                        VIDEO_SUBLAYER_IDENTITY_TAG | id.0,
                        live_content
                            .video_sinks
                            .get(&id.0)
                            .is_some_and(|sink| sink.frame.is_some() && sink.frame_has_surface),
                    ),
                    LiveMedia::ImageFrame(_)
                    | LiveMedia::CompositedContext(_)
                    | LiveMedia::Caret
                    | LiveMedia::ScrollBar { .. } => return,
                };
                if !has_content {
                    return;
                }
                sublayers.push(PlannedSublayer {
                    identity: sublayer_identity_of_media
                        | (u64::from(ordinal) << MEDIA_ORDINAL_SHIFT_PAST_CANVAS_AND_VIDEO_IDS),
                    group_index,
                    content: SublayerContent::Placed {
                        content: planned_content,
                        placement: placement.clone(),
                    },
                });
                return;
            }
            if let LayerContent::OwnSurface { .. } = layer.content {
                if let Some((surface, rect_in_layer_space)) = self.layers.tiles.own_surface_of(&compositing.identity) {
                    let draw = TileDraw::TileSurface {
                        coord: TileCoord::new(0, 0),
                        surface,
                        every_pixel_is_opaque: false,
                        rect_in_layer_space,
                        rect_in_surface: rect_of_size(size_of(rect_in_layer_space)),
                    };
                    self.append_sublayers_of_tiled_layer(
                        &mut sublayers,
                        compositing,
                        vec![draw],
                        group_index,
                        raster_moves_by_whole_pixels_only,
                    );
                }
                return;
            }
            let draws = self.layers.tiles.draws_reaching_into(compositing, rect_of_raster_shown);
            self.append_sublayers_of_tiled_layer(
                &mut sublayers,
                compositing,
                draws,
                group_index,
                raster_moves_by_whole_pixels_only,
            );
        });
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
