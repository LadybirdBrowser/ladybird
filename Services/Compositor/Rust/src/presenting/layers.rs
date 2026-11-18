/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The layers a frame is painted in while it draws canvases, videos or nested contexts, and what of them the window
//! server shows above the frame by itself.

use super::damage::LiveContentChangesSinceLastPaint;
use super::{
    ContextPresenting, FrameToPaint, PresentingEnvironment, paint_overlays, rect_of_size, replay_into, replaying_only,
};
use crate::raster_host::{
    FfiSublayer, FfiSublayerPlacement, LiveContent, MediaKind, PresentableSurfaceHandle, SublayerContentKind,
    SurfaceBlend, SurfaceHandle,
};
use libcompositing_rust::display_list::layers::{LayerKind, LayerPlan, LayerPlanInputs, build_layer_plan};
use libcompositing_rust::display_list::summary::LiveContent as LiveMedia;
use libcompositing_rust::display_list::window_server_placement::WindowServerPlacement;
use libgfx_rust::{Color, IntRect, IntSize, ScalingMode};
use std::collections::HashMap;

// The pool of presentable surfaces the overlays are shown from, apart from those of the painted layers.
const COMPOSITOR_OVERLAY_SURFACE_POOL: u32 = u32::MAX;

// Sublayers keep their identity from one frame to the next by what they show.
const CANVAS_SUBLAYER_IDENTITY_TAG: u64 = 1 << 60;
const VIDEO_SUBLAYER_IDENTITY_TAG: u64 = 2 << 60;
const PAINTED_LAYER_SUBLAYER_IDENTITY_TAG: u64 = 3 << 60;
const COMPOSITOR_OVERLAY_SUBLAYER_IDENTITY: u64 = 4 << 60;
// Canvas and video IDs count up from 1, and stay far below this.
const MEDIA_DRAWING_INDEX_SHIFT: u32 = 44;

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
    plan: Option<LayerPlan>,
    surfaces: Vec<LayerSurface>,
    last_content_version: u64,
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

    pub fn window_server_shows_media(&self) -> bool {
        self.window_server_shows_media
    }

    pub fn drop_layers(&mut self, env: &PresentingEnvironment<'_>) {
        if self.plan.is_none() && self.surfaces.is_empty() && self.painted_layer_presentations.is_empty() {
            return;
        }
        self.plan = None;
        self.surfaces.clear();
        for pool in 0..self.painted_layer_presentations.len() {
            env.raster.drop_presentable_surface_pool(env.context_id, pool as u32);
        }
        self.painted_layer_presentations.clear();
    }
}

/// The layers a frame is painted in, and what of each it repaints.
#[derive(Default)]
pub struct LayerUpdate {
    pub new_plan: Option<LayerPlan>,
    pub keeps_plan: bool,
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

pub fn rect_of_media_layers_showing_changes(plan: &LayerPlan, changes: &LiveContentChangesSinceLastPaint) -> IntRect {
    let mut rect = IntRect::default();
    for layer in &plan.layers {
        if let LayerKind::Media { content } = layer.kind
            && changes.changed(content)
        {
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
    pub(super) fn layer_update_for(&self, frame: &FrameToPaint<'_>, viewport_rect: IntRect) -> LayerUpdate {
        let mut layers = LayerUpdate::default();
        let draws_media = frame
            .display_list
            .storage()
            .summary()
            .live_content_records
            .iter()
            .any(|live_content_record| !matches!(live_content_record.content, LiveMedia::ImageFrame(_)));
        if !draws_media {
            return layers;
        }
        if self.layers_keep_plan_for(frame, viewport_rect) {
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
                LiveMedia::ImageFrame(_) | LiveMedia::CompositedContext(_) => return false,
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
        if !layer_update.has_plan() {
            layers.drop_layers(env);
            return;
        }
        if let Some(new_plan) = layer_update.new_plan {
            layers.plan = Some(new_plan);
        }
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
        let surface_clear_color = frame.display_list.surface_clear_color().unwrap_or(Color::TRANSPARENT);
        raster.start_raster_cache_frame(env.context_id);
        let layer_count = plan.layers.len();

        let mut surfaces = std::mem::take(&mut self.layers.surfaces);
        surfaces.resize_with(layer_count, LayerSurface::default);
        for (index, layer) in plan.layers.iter().enumerate() {
            let layer_surface = &mut surfaces[index];
            let is_base = matches!(layer.kind, LayerKind::Base);
            let rect = if is_base {
                rect_of_size(self.raster_size())
            } else {
                self.raster_rect_of(layer.rect)
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
                    false,
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
                self.raster_rect_of(layer_update.damage_of_layers[index])
                    .intersected(layer_surface.rect)
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
            self.apply_raster_transform(raster, target);
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
            self.apply_raster_transform(raster, surface);
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
                if !self.raster_rect_of(layer.rect).intersected(damage_rect).is_empty() {
                    let save_count = env.raster.save(surface);
                    self.apply_raster_transform(env.raster, surface);
                    replay_into(env, surface, frame, replaying_only(&layer.record_spans, true));
                    env.raster.restore_to_count(surface, save_count);
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
                    LiveMedia::ImageFrame(_) | LiveMedia::CompositedContext(_) => continue,
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
