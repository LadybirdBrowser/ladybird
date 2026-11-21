/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the window server shows above a frame by itself: the media layers it can show, and the surfaces it shows
//! painted layers and the overlays of the compositor from.

use super::layers::{LayerCompositingInFrame, PaintedLayers};
use super::{ContextPresenting, FrameToPaint, PresentingEnvironment, rect_of_size, size_of};
use crate::raster_host::{FfiSublayer, FfiSublayerPlacement, LiveContent, MediaKind, SublayerContentKind};
use crate::surfaces::painting_surface::PaintingSurface;
use crate::surfaces::presentable_surface::{PresentableSurface, PresentableSurfacePool};
use libcompositing_rust::display_list::layers::{
    LayerContent, LayerIdentity, LayerIndex, StackLayer, WindowServerPlacementsOfMedia,
};
use libcompositing_rust::display_list::summary::LiveContent as LiveMedia;
use libcompositing_rust::display_list::window_server_placement::{
    WhyWindowServerCannotShowMedia, WindowServerPlacement,
};
use libgfx_rust::{Color, IntRect, ScalingMode};
use std::collections::{HashMap, HashSet};
use std::rc::Rc;

const CANVAS_SUBLAYER_IDENTITY_TAG: u64 = 1 << 60;
const VIDEO_SUBLAYER_IDENTITY_TAG: u64 = 2 << 60;
const PAINTED_LAYER_SUBLAYER_IDENTITY_TAG: u64 = 3 << 60;
const COMPOSITOR_OVERLAY_SUBLAYER_IDENTITY: u64 = 4 << 60;
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
    next_sublayer_ordinal: u32,
}

impl WindowServerLayers {
    pub(super) fn keep_only_layers(&mut self, identities_in_stack: &HashSet<LayerIdentity>) {
        self.presentation_by_layer
            .retain(|identity, _| identities_in_stack.contains(identity));
    }
}

fn sublayer_placement_of(placement: &WindowServerPlacement) -> FfiSublayerPlacement {
    let rounded_clip = placement.rounded_clip;
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
        scaling_mode: placement.scaling_mode,
    }
}

fn unclipped_untransformed_surface_sublayer(
    identity: u64,
    surface: &Rc<PresentableSurface>,
    rect_in_frame: IntRect,
) -> FfiSublayer {
    FfiSublayer {
        identity,
        content_kind: SublayerContentKind::PresentableSurface,
        media_id: 0,
        presentable_surface: surface.as_ffi(),
        placement: FfiSublayerPlacement {
            content_rect: rect_in_frame,
            content_to_frame: [1.0, 0.0, 0.0, 1.0, 0.0, 0.0],
            clip_rect: rect_in_frame.to_float(),
            scaling_mode: ScalingMode::NearestNeighbor,
            ..Default::default()
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
        None
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

    pub(super) fn present_compositor_overlays_to_window_server(
        &mut self,
        env: &PresentingEnvironment<'_>,
        frame: &FrameToPaint<'_>,
        like: &Rc<PaintingSurface>,
    ) -> bool {
        let raster = env.raster;
        let mut overlays_rect = frame.frame_inputs.overlays.bounds;
        if self.layer_overlay.latest_frame().is_some() {
            overlays_rect = overlays_rect.united(self.raster_rect());
        }
        if overlays_rect.is_empty() {
            self.layers.window_server.latest_compositor_overlays_surface_and_rect = None;
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
        true
    }

    pub(super) fn sublayers_shown_by_window_server(&self, live_content: &LiveContent) -> Vec<FfiSublayer> {
        let layers = &self.layers.window_server;
        let mut sublayers = Vec::new();
        let Some(painted) = self
            .painted_frame
            .as_ref()
            .map(|painted_frame| &painted_frame.layers)
            .filter(|painted| painted.shown_by_window_server())
        else {
            return sublayers;
        };
        for compositing in painted
            .compositing_of_layers
            .iter()
            .skip(1)
            .filter(|compositing| compositing.shown_by_window_server)
            .filter(|compositing| !self.layer_overlay.hides_layer(&compositing.identity))
        {
            let layer_index = compositing.layer_index;
            let layer = compositing.layer_in(&painted.stack);
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
                    identity: sublayer_identity_of_media
                        | (u64::from(ordinal) << MEDIA_ORDINAL_SHIFT_PAST_CANVAS_AND_VIDEO_IDS),
                    content_kind: kind,
                    media_id: id,
                    presentable_surface: std::ptr::null(),
                    placement: sublayer_placement_of(placement),
                });
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
