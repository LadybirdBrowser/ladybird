/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The layers a frame is painted in while canvases and videos update often, and what of them the window server shows
//! above the frame by itself.

use super::{
    ContextPresenting, FrameToPaint, PresentingEnvironment, paint_overlays, rect_of_size, replay_into, replaying_only,
};
use crate::host::ContextTimer;
use crate::raster_host::{
    FfiSublayer, FfiSublayerPlacement, LiveContent, MediaKind, PresentableSurfaceHandle, SublayerContentKind,
    SurfaceBlend, SurfaceHandle,
};
use libcompositing_rust::display_list::commands::{CanvasId, VideoSinkResourceId};
use libcompositing_rust::display_list::layers::{LayerKind, LayerPlan, LayerPlanInputs, build_layer_plan};
use libcompositing_rust::display_list::summary::LiveContent as LiveMedia;
use libcompositing_rust::display_list::window_server_placement::WindowServerPlacement;
use libcompositing_rust::time::{Duration, MonotonicTime};
use libgfx_rust::{Color, IntRect, IntSize, ScalingMode};
use std::collections::HashMap;

const LONGEST_TIME_BETWEEN_UPDATES_THAT_ACTIVATE_MEDIA: Duration = Duration::from_milliseconds(1000);
const TIME_MEDIA_STAYS_ACTIVE_AFTER_AN_UPDATE: Duration = Duration::from_milliseconds(3000);

// The pool of presentable surfaces the overlays are shown from, apart from those of the painted layers.
const COMPOSITOR_OVERLAY_SURFACE_POOL: u32 = u32::MAX;

// Sublayers keep their identity from one frame to the next by what they show.
const CANVAS_SUBLAYER_IDENTITY_TAG: u64 = 1 << 60;
const VIDEO_SUBLAYER_IDENTITY_TAG: u64 = 2 << 60;
const PAINTED_LAYER_SUBLAYER_IDENTITY_TAG: u64 = 3 << 60;
const COMPOSITOR_OVERLAY_SUBLAYER_IDENTITY: u64 = 4 << 60;
// Canvas and video IDs count up from 1, and stay far below this.
const MEDIA_DRAWING_INDEX_SHIFT: u32 = 44;

pub struct MediaUpdate {
    pub media: LiveMedia,
    pub content_version: u64,
}

struct MediaUpdates {
    media: LiveMedia,
    content_version: u64,
    last_update: MonotonicTime,
    active: bool,
}

/// Which canvases and videos update often enough to be layers of their own: those updated twice within a second, for
/// three seconds after their last update.
#[derive(Default)]
pub struct MediaActivity {
    updates: Vec<MediaUpdates>,
}

impl MediaActivity {
    pub fn note_updates(&mut self, updates: &[MediaUpdate], now: MonotonicTime) {
        self.updates
            .retain(|media_updates| now - media_updates.last_update <= TIME_MEDIA_STAYS_ACTIVE_AFTER_AN_UPDATE);
        for update in updates {
            self.note_update(update, now);
        }
    }

    fn note_update(&mut self, update: &MediaUpdate, now: MonotonicTime) {
        for updates in &mut self.updates {
            if updates.media != update.media {
                continue;
            }
            if updates.content_version == update.content_version {
                return;
            }
            if now - updates.last_update <= LONGEST_TIME_BETWEEN_UPDATES_THAT_ACTIVATE_MEDIA {
                updates.active = true;
            }
            updates.content_version = update.content_version;
            updates.last_update = now;
            return;
        }
        self.updates.push(MediaUpdates {
            media: update.media,
            content_version: update.content_version,
            last_update: now,
            active: false,
        });
    }

    pub fn active_media(&self, now: MonotonicTime) -> Vec<LiveMedia> {
        self.updates
            .iter()
            .filter(|updates| updates.active && now - updates.last_update <= TIME_MEDIA_STAYS_ACTIVE_AFTER_AN_UPDATE)
            .map(|updates| updates.media)
            .collect()
    }
}

#[derive(Default)]
struct LayerSurface {
    surface: Option<SurfaceHandle>,
    rect: IntRect,
    // Changes whenever anything is drawn into the surface.
    content_version: u64,
}

// The surface that the window server shows a painted layer from.
#[derive(Default)]
struct PaintedLayerPresentation {
    latest_surface: Option<PresentableSurfaceHandle>,
    rect: IntRect,
    content_version: u64,
}

/// The layers of the frames of a context, and what of them the window server shows.
#[derive(Default)]
pub struct ContextLayers {
    pub media_activity: MediaActivity,
    plan: Option<LayerPlan>,
    plan_active_media: Vec<LiveMedia>,
    surfaces: Vec<LayerSurface>,
    last_content_version: u64,
    pub repaint_everything_after_dropping_layers: bool,
    // Whether the window server shows the canvases and videos of the latest frame above it.
    window_server_shows_media: bool,
    painted_layer_presentations: Vec<PaintedLayerPresentation>,
    latest_overlay_surface: Option<PresentableSurfaceHandle>,
    overlay_rect: IntRect,
}

impl ContextLayers {
    pub fn has_plan(&self) -> bool {
        self.plan.is_some()
    }

    pub fn plan(&self) -> Option<&LayerPlan> {
        self.plan.as_ref()
    }

    pub fn plan_active_media(&self) -> &[LiveMedia] {
        &self.plan_active_media
    }

    pub fn window_server_shows_media(&self) -> bool {
        self.window_server_shows_media
    }

    pub fn drop_layers(&mut self, env: &PresentingEnvironment<'_>) {
        // NB: The drop timer runs only while there is a plan.
        if self.plan.is_none() && self.surfaces.is_empty() && self.painted_layer_presentations.is_empty() {
            return;
        }
        self.plan = None;
        self.plan_active_media.clear();
        self.surfaces.clear();
        for pool in 0..self.painted_layer_presentations.len() {
            env.raster.drop_presentable_surface_pool(env.context_id, pool as u32);
        }
        self.painted_layer_presentations.clear();
        env.host.cancel_timer(env.context_id, ContextTimer::LayerDrop);
    }

    pub fn drop_timer_fired(&mut self, env: &PresentingEnvironment<'_>) {
        // While the window server shows the media, the layers cost nothing, and the frame on screen still needs their
        // plan, as for a snapshot. The next frame without a plan puts the media back into the store.
        if self.plan.is_none() || self.window_server_shows_media {
            return;
        }
        self.drop_layers(env);
        self.repaint_everything_after_dropping_layers = true;
    }
}

/// The layers a frame is painted in, and what of each it repaints.
#[derive(Default)]
pub struct LayerUpdate {
    pub new_plan: Option<LayerPlan>,
    pub keeps_plan: bool,
    pub active_media: Vec<LiveMedia>,
    pub painted_damage: IntRect,
    pub damage_of_layers: Vec<IntRect>,
    pub fully_repainted_layer_bits: u32,
    pub repaints_everything: bool,
    pub shown_by_window_server: bool,
}

impl LayerUpdate {
    pub fn has_plan(&self) -> bool {
        self.new_plan.is_some() || self.keeps_plan
    }

    pub fn plan<'a>(&'a self, layers: &'a ContextLayers) -> Option<&'a LayerPlan> {
        if self.keeps_plan {
            layers.plan.as_ref()
        } else {
            self.new_plan.as_ref()
        }
    }
}

pub fn rect_of_media_layers_showing(
    plan: &LayerPlan,
    canvases: &[CanvasId],
    video_sinks: &[VideoSinkResourceId],
) -> IntRect {
    let mut rect = IntRect::default();
    for layer in &plan.layers {
        let LayerKind::Media { content } = layer.kind else {
            continue;
        };
        let shows_changed_media = match content {
            LiveMedia::Canvas(id) => canvases.contains(&id),
            LiveMedia::VideoFrame(id) => video_sinks.contains(&id),
            LiveMedia::ImageFrame(_) => false,
        };
        if shows_changed_media {
            rect = rect.united(layer.rect);
        }
    }
    rect
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

// A sublayer that shows a surface at its own rect, unclipped and untransformed.
fn surface_sublayer(identity: u64, surface: &PresentableSurfaceHandle, rect: IntRect) -> FfiSublayer {
    FfiSublayer {
        identity,
        content_kind: SublayerContentKind::PresentableSurface,
        media_id: 0,
        presentable_surface: surface.as_ptr(),
        placement: FfiSublayerPlacement {
            content_rect: rect,
            content_to_frame: [1.0, 0.0, 0.0, 1.0, 0.0, 0.0],
            clip_rect: rect.to_float(),
            scaling_mode: ScalingMode::NearestNeighbor,
            ..Default::default()
        },
    }
}

impl ContextPresenting {
    pub(super) fn layer_update_for(
        &self,
        env: &PresentingEnvironment<'_>,
        frame: &FrameToPaint<'_>,
        viewport_rect: IntRect,
        live_content: &LiveContent,
    ) -> LayerUpdate {
        let mut layers = LayerUpdate::default();
        if !self.presents_to_client {
            return layers;
        }
        layers.active_media = self.layers.media_activity.active_media(env.now);
        // An HDR video is a layer from its first frame for as long as it is drawn, even while paused, so that the window
        // server keeps showing it brighter than SDR white instead of it going back into the frame, which holds SDR.
        if self.remote_layer_may_show_media(env.has_gpu)
            && env.raster.remote_layer_can_show_high_dynamic_range(env.context_id)
            && env.window_server_may_show_high_dynamic_range
        {
            for id in &frame.display_list.storage().summary().video_sink_ids {
                let media = LiveMedia::VideoFrame(VideoSinkResourceId(*id));
                if layers.active_media.contains(&media) {
                    continue;
                }
                let holds_high_dynamic_range = live_content
                    .video_sinks
                    .get(id)
                    .is_some_and(|sink| sink.has_frame && sink.frame_holds_high_dynamic_range);
                if holds_high_dynamic_range {
                    layers.active_media.push(media);
                }
            }
        }
        if layers.active_media.is_empty() {
            return layers;
        }
        if self.layers_keep_plan_for(frame, viewport_rect, &layers.active_media) {
            layers.keeps_plan = true;
            return layers;
        }
        let storage = frame.display_list.storage();
        let tree = frame.frame_inputs.visual_context_tree.as_arc();
        layers.new_plan = build_layer_plan(&LayerPlanInputs {
            tape: storage.bytes(),
            command_runs: storage.command_runs(),
            summary: storage.summary(),
            effect_clips: storage.effect_clip_plan(tree),
            tree,
            scroll_offsets: frame.frame_inputs.presented_scroll_state_snapshot.device_offsets(),
            viewport_rect,
            active_media: &layers.active_media,
        })
        .ok();
        layers
    }

    pub(super) fn window_server_can_show_media_layers_of(
        &self,
        env: &PresentingEnvironment<'_>,
        plan: &LayerPlan,
        live_content: &LiveContent,
    ) -> bool {
        if !self.remote_layer_may_show_media(env.has_gpu) {
            return false;
        }
        for layer in &plan.layers {
            let LayerKind::Media { content } = layer.kind else {
                continue;
            };
            let Some(Ok(placement)) = &layer.window_server_placement else {
                return false;
            };
            let (kind, id) = match content {
                LiveMedia::Canvas(id) => {
                    let has_presentable_surface = live_content
                        .canvases
                        .get(&id.0)
                        .is_some_and(|canvas| canvas.has_presentable_surface);
                    if !has_presentable_surface {
                        return false;
                    }
                    (MediaKind::Canvas, id.0)
                }
                LiveMedia::VideoFrame(id) => {
                    let Some(sink) = live_content.video_sinks.get(&id.0).filter(|sink| sink.has_frame) else {
                        return false;
                    };
                    if !env.window_server_may_show_high_dynamic_range && sink.frame_holds_high_dynamic_range {
                        return false;
                    }
                    (MediaKind::Video, id.0)
                }
                LiveMedia::ImageFrame(_) => return false,
            };
            if !env.raster.remote_layer_can_show(
                env.context_id,
                kind,
                id,
                &sublayer_placement_of(placement),
                self.device_pixel_ratio,
            ) {
                return false;
            }
        }
        true
    }

    pub(super) fn commit_layer_update(&mut self, env: &PresentingEnvironment<'_>, layer_update: LayerUpdate) {
        let layers = &mut self.layers;
        layers.window_server_shows_media = layer_update.has_plan() && layer_update.shown_by_window_server;
        if layer_update.repaints_everything {
            layers.repaint_everything_after_dropping_layers = false;
        }
        if !layer_update.has_plan() {
            layers.drop_layers(env);
            return;
        }
        if let Some(new_plan) = layer_update.new_plan {
            layers.plan = Some(new_plan);
        }
        layers.plan_active_media = layer_update.active_media;
        env.host.set_timer(
            env.context_id,
            ContextTimer::LayerDrop,
            TIME_MEDIA_STAYS_ACTIVE_AFTER_AN_UPDATE,
        );
    }

    pub(super) fn paint_layered_frame(
        &mut self,
        env: &PresentingEnvironment<'_>,
        surface: &SurfaceHandle,
        frame: &FrameToPaint<'_>,
        damage_rect: IntRect,
        layer_update: &LayerUpdate,
    ) -> bool {
        // The plan a frame keeps stays with the layers, out of their way while they are painted.
        let kept_plan = if layer_update.keeps_plan {
            self.layers.plan.take()
        } else {
            None
        };
        let plan = kept_plan
            .as_ref()
            .or(layer_update.new_plan.as_ref())
            .expect("a layered frame has a plan");
        let painted = self.paint_layers(env, surface, frame, damage_rect, layer_update, plan);
        if layer_update.keeps_plan {
            self.layers.plan = kept_plan;
        }
        painted
    }

    fn paint_layers(
        &mut self,
        env: &PresentingEnvironment<'_>,
        surface: &SurfaceHandle,
        frame: &FrameToPaint<'_>,
        damage_rect: IntRect,
        layer_update: &LayerUpdate,
        plan: &LayerPlan,
    ) -> bool {
        let raster = env.raster;
        let viewport_size = self.viewport_size;
        let surface_clear_color = frame.display_list.surface_clear_color().unwrap_or(Color::TRANSPARENT);
        raster.start_raster_cache_frame(env.context_id);
        let layer_count = plan.layers.len();

        let mut surfaces = std::mem::take(&mut self.layers.surfaces);
        surfaces.resize_with(layer_count, LayerSurface::default);
        for (index, layer) in plan.layers.iter().enumerate() {
            let layer_surface = &mut surfaces[index];
            let is_base = matches!(layer.kind, LayerKind::Base);
            let rect = if is_base {
                rect_of_size(viewport_size)
            } else {
                layer.rect
            };
            if matches!(layer.kind, LayerKind::Media { .. }) || rect.is_empty() {
                *layer_surface = LayerSurface::default();
                continue;
            }
            let mut repaints_whole_layer = (layer_update.fully_repainted_layer_bits & (1 << index)) != 0;
            let surface_fits = if is_base {
                layer_surface.rect == rect
            } else {
                layer_surface.rect.contains_rect(rect)
            };
            let shares_backend = layer_surface
                .surface
                .as_ref()
                .is_some_and(|layer_surface| raster.surfaces_share_backend(layer_surface, surface));
            if layer_surface.surface.is_none() || !surface_fits || !shares_backend {
                let Some(new_surface) = raster.create_surface(
                    IntSize {
                        width: rect.width,
                        height: rect.height,
                    },
                    Some(surface),
                ) else {
                    self.layers.surfaces = surfaces;
                    return false;
                };
                *layer_surface = LayerSurface {
                    surface: Some(new_surface),
                    rect,
                    content_version: 0,
                };
                repaints_whole_layer = true;
            }
            let repaint_rect = if repaints_whole_layer {
                layer_surface.rect
            } else {
                layer_update.damage_of_layers[index].intersected(layer_surface.rect)
            };
            if repaint_rect.is_empty() {
                continue;
            }
            let target = layer_surface.surface.as_ref().expect("the layer has a surface");
            let save_count = raster.save(target);
            raster.translate(target, -layer_surface.rect.x as f32, -layer_surface.rect.y as f32);
            raster.clip_rect(target, repaint_rect);
            raster.clear(
                target,
                if is_base {
                    surface_clear_color
                } else {
                    Color::TRANSPARENT
                },
            );
            replay_into(env, target, frame, replaying_only(&layer.record_spans, false));
            raster.restore_to_count(target, save_count);
            self.layers.last_content_version += 1;
            layer_surface.content_version = self.layers.last_content_version;
        }
        self.layers.surfaces = surfaces;

        let save_count = raster.save(surface);
        raster.clip_rect(surface, damage_rect);
        if let Some(base_surface) = self.layers.surfaces.first()
            && let Some(base) = &base_surface.surface
            && base_surface.rect.intersected(damage_rect) != IntRect::default()
        {
            raster.draw_surface(
                surface,
                base,
                IntRect::new(0, 0, base_surface.rect.width, base_surface.rect.height),
                base_surface.rect,
                SurfaceBlend::Source,
            );
        }
        if !layer_update.shown_by_window_server {
            self.composite_layers_above_base_with_plan(env, surface, frame, plan, damage_rect);
            paint_overlays(env, surface, frame.frame_inputs, true);
        }
        raster.restore_to_count(surface, save_count);

        // The window server shows everything above the base layer from surfaces of its own.
        if layer_update.shown_by_window_server {
            if !self.present_painted_layers_to_window_server(env, plan, surface) {
                return false;
            }
            if !self.present_compositor_overlays_to_window_server(env, frame, surface) {
                return false;
            }
        }
        true
    }

    pub(super) fn composite_layers_above_base(
        &self,
        env: &PresentingEnvironment<'_>,
        surface: &SurfaceHandle,
        frame: &FrameToPaint<'_>,
        damage_rect: IntRect,
    ) {
        let Some(plan) = self.layers.plan() else {
            return;
        };
        self.composite_layers_above_base_with_plan(env, surface, frame, plan, damage_rect);
    }

    fn composite_layers_above_base_with_plan(
        &self,
        env: &PresentingEnvironment<'_>,
        surface: &SurfaceHandle,
        frame: &FrameToPaint<'_>,
        plan: &LayerPlan,
        damage_rect: IntRect,
    ) {
        for (index, layer) in plan.layers.iter().enumerate().skip(1) {
            if matches!(layer.kind, LayerKind::Media { .. }) {
                if !layer.rect.intersected(damage_rect).is_empty() {
                    replay_into(env, surface, frame, replaying_only(&layer.record_spans, true));
                }
                continue;
            }
            let Some(layer_surface) = self.layers.surfaces.get(index) else {
                continue;
            };
            let Some(source) = &layer_surface.surface else {
                continue;
            };
            if layer_surface.rect.intersected(damage_rect).is_empty() {
                continue;
            }
            env.raster.draw_surface(
                surface,
                source,
                IntRect::new(0, 0, layer_surface.rect.width, layer_surface.rect.height),
                layer_surface.rect,
                SurfaceBlend::SourceOver,
            );
        }
    }

    // Each painted layer goes to the window server as its own surface, which is copied from its raster when the raster
    // changes. The window server may still show the surface of the frame before, so the copy goes into another one.
    fn present_painted_layers_to_window_server(
        &mut self,
        env: &PresentingEnvironment<'_>,
        plan: &LayerPlan,
        like: &SurfaceHandle,
    ) -> bool {
        let raster = env.raster;
        let layers = &mut self.layers;
        // The surfaces of the layers that the plan no longer has go with them.
        for pool in plan.layers.len()..layers.painted_layer_presentations.len() {
            raster.drop_presentable_surface_pool(env.context_id, pool as u32);
        }
        layers
            .painted_layer_presentations
            .resize_with(plan.layers.len(), PaintedLayerPresentation::default);
        for (index, layer) in plan.layers.iter().enumerate() {
            let presentation = &mut layers.painted_layer_presentations[index];
            let layer_surface = layers.surfaces.get(index);
            let source = layer_surface.and_then(|layer_surface| layer_surface.surface.as_ref());
            let (Some(layer_surface), Some(source)) = (layer_surface, source) else {
                presentation.latest_surface = None;
                continue;
            };
            if !matches!(layer.kind, LayerKind::Painted) || layer.rect.is_empty() {
                presentation.latest_surface = None;
                continue;
            }
            if presentation.latest_surface.is_some()
                && presentation.rect == layer.rect
                && presentation.content_version == layer_surface.content_version
            {
                continue;
            }
            let layer_size = IntSize {
                width: layer.rect.width,
                height: layer.rect.height,
            };
            let Some(presentable) = raster.next_presentable_surface(env.context_id, index as u32, layer_size, like)
            else {
                return false;
            };
            let target = raster.painting_surface_of(&presentable);
            let source_rect = IntRect::new(
                layer.rect.x - layer_surface.rect.x,
                layer.rect.y - layer_surface.rect.y,
                layer.rect.width,
                layer.rect.height,
            );
            raster.draw_surface(
                &target,
                source,
                source_rect,
                rect_of_size(layer_size),
                SurfaceBlend::Source,
            );
            presentation.latest_surface = Some(presentable);
            presentation.rect = layer.rect;
            presentation.content_version = layer_surface.content_version;
        }
        true
    }

    fn present_compositor_overlays_to_window_server(
        &mut self,
        env: &PresentingEnvironment<'_>,
        frame: &FrameToPaint<'_>,
        like: &SurfaceHandle,
    ) -> bool {
        let raster = env.raster;
        let bounds = frame.frame_inputs.overlays.bounds;
        self.layers.overlay_rect = bounds;
        if bounds.is_empty() {
            self.layers.latest_overlay_surface = None;
            return true;
        }
        let bounds_size = IntSize {
            width: bounds.width,
            height: bounds.height,
        };
        let Some(presentable) =
            raster.next_presentable_surface(env.context_id, COMPOSITOR_OVERLAY_SURFACE_POOL, bounds_size, like)
        else {
            return false;
        };
        let target = raster.painting_surface_of(&presentable);
        raster.clear(&target, Color::TRANSPARENT);
        let save_count = raster.save(&target);
        raster.translate(&target, -bounds.x as f32, -bounds.y as f32);
        paint_overlays(env, &target, frame.frame_inputs, true);
        raster.restore_to_count(&target, save_count);
        self.layers.latest_overlay_surface = Some(presentable);
        true
    }

    pub(super) fn sublayers_shown_by_window_server(&self, live_content: &LiveContent) -> Vec<FfiSublayer> {
        let layers = &self.layers;
        let mut sublayers = Vec::new();
        let Some(plan) = layers.plan.as_ref().filter(|_| layers.window_server_shows_media) else {
            return sublayers;
        };
        // The same canvas or video can be drawn by more than one record, and each drawing is a sublayer of its own.
        let mut drawings_of_media: HashMap<u64, u64> = HashMap::new();
        for (index, layer) in plan.layers.iter().enumerate().skip(1) {
            if let LayerKind::Media { content } = layer.kind {
                let Some(Ok(placement)) = &layer.window_server_placement else {
                    continue;
                };
                let (kind, id, mut identity, has_content) = match content {
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
                    LiveMedia::ImageFrame(_) => continue,
                };
                if !has_content {
                    continue;
                }
                let earlier_drawings = drawings_of_media.entry(identity).or_insert(0);
                identity |= *earlier_drawings << MEDIA_DRAWING_INDEX_SHIFT;
                *earlier_drawings += 1;
                sublayers.push(FfiSublayer {
                    identity,
                    content_kind: kind,
                    media_id: id,
                    presentable_surface: std::ptr::null_mut(),
                    placement: sublayer_placement_of(placement),
                });
                continue;
            }
            let Some(presentation) = layers.painted_layer_presentations.get(index) else {
                continue;
            };
            let Some(surface) = &presentation.latest_surface else {
                continue;
            };
            sublayers.push(surface_sublayer(
                PAINTED_LAYER_SUBLAYER_IDENTITY_TAG | index as u64,
                surface,
                presentation.rect,
            ));
        }
        if let Some(surface) = &layers.latest_overlay_surface {
            sublayers.push(surface_sublayer(
                COMPOSITOR_OVERLAY_SUBLAYER_IDENTITY,
                surface,
                layers.overlay_rect,
            ));
        }
        sublayers
    }
}
