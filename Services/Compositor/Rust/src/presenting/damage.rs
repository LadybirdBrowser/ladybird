/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a frame repaints of the frame painted before it, and of what each backing store holds.

use super::layers::{LayerUpdate, MediaUpdate};
use super::{ContextPresenting, FrameToPaint, PresentingEnvironment, rect_of_size};
use crate::frame_inputs::FrameInputs;
use crate::host::PendingFrame;
use crate::raster_host::LiveContent;
use libcompositing_rust::display_list::commands::{CanvasId, ImageFrameResourceId, VideoSinkResourceId};
use libcompositing_rust::display_list::damage::{DisplayListDamage, LayerPlans, LiveContentChanges};
use libcompositing_rust::display_list::summary::LiveContent as LiveMedia;
use libcompositing_rust::scrolling::DisplayList;
use libgfx_rust::{IntRect, IntSize};
use std::collections::{HashMap, HashSet};
use std::rc::Rc;

/// The frame painted last: its display list and inputs, and the version of each live content it drew, which comparing
/// lists cannot see change.
pub struct PaintedFrame {
    pub display_list: Rc<DisplayList>,
    pub frame_inputs: FrameInputs,
    pub viewport_size: IntSize,
    canvas_content_generations: HashMap<u64, u64>,
    video_frame_ids: HashMap<u64, Option<u64>>,
    image_frame_pixel_generations: HashMap<u64, u64>,
}

impl PaintedFrame {
    pub fn draws_canvas(&self, canvas_id: u64) -> bool {
        self.canvas_content_generations.contains_key(&canvas_id)
    }
}

/// What each backing store misses of the frames painted since it was last painted into, by its id, which new stores
/// never repeat. The stores painted into least recently are forgotten first, as retired stores are never painted into
/// again, and a store that is forgotten is painted in full.
#[derive(Default)]
pub struct StoreDamage {
    // The store painted into last is last.
    accumulated: Vec<(i32, IntRect)>,
}

// Twice as many as a context ever has at once.
const MAXIMUM_REMEMBERED_STORE_COUNT: usize = 16;

impl StoreDamage {
    pub fn forget_every_store(&mut self) {
        self.accumulated.clear();
    }

    /// What painting a frame into the store repaints: the frame's damage, and what the store missed since it was last
    /// painted, all of it for a store never painted into.
    pub fn take(&mut self, bitmap_id: i32, frame_damage: IntRect, store_rect: IntRect) -> IntRect {
        let mut repaint_rect = store_rect;
        for (id, accumulated) in &mut self.accumulated {
            if *id == bitmap_id {
                repaint_rect = accumulated.united(frame_damage);
            }
            *accumulated = accumulated.united(frame_damage);
        }
        self.accumulated.retain(|(id, _)| *id != bitmap_id);
        self.accumulated.push((bitmap_id, IntRect::default()));
        if self.accumulated.len() > MAXIMUM_REMEMBERED_STORE_COUNT {
            self.accumulated.remove(0);
        }
        repaint_rect
    }
}

// The canvases, video sinks and image frames whose pixels changed since the frame painted last.
#[derive(Default)]
pub struct LiveContentChangesSinceLastPaint {
    pub canvases: Vec<CanvasId>,
    pub video_sinks: Vec<VideoSinkResourceId>,
    pub image_frames: Vec<ImageFrameResourceId>,
    pub media_updates: Vec<MediaUpdate>,
    pub repaints_everything: bool,
}

pub struct FrameDamage {
    pub rect: IntRect,
    pub layers: LayerUpdate,
    pub live_media_changed: bool,
}

impl ContextPresenting {
    /// The live content of the context as it is now, with the canvases of the list and of the frame painted last.
    pub(super) fn live_content(&self, env: &PresentingEnvironment<'_>, display_list: &DisplayList) -> LiveContent {
        let mut canvas_ids = display_list.storage().summary().canvas_ids.clone();
        if let Some(painted_frame) = &self.painted_frame {
            let canvas_ids_of_list: HashSet<u64> = canvas_ids.iter().copied().collect();
            canvas_ids.extend(
                painted_frame
                    .canvas_content_generations
                    .keys()
                    .filter(|id| !canvas_ids_of_list.contains(id)),
            );
        }
        env.raster.live_content(env.context_id, &canvas_ids)
    }

    pub(super) fn remember_painted_frame(
        &mut self,
        viewport_rect: IntRect,
        display_list: &Rc<DisplayList>,
        frame_inputs: FrameInputs,
        live_content: &LiveContent,
    ) {
        let canvas_generation = |id: u64| {
            live_content
                .canvases
                .get(&id)
                .map_or(0, |canvas| canvas.content_generation)
        };
        let canvas_content_generations = display_list
            .storage()
            .summary()
            .canvas_ids
            .iter()
            .map(|id| (*id, canvas_generation(*id)))
            .collect();
        // Every video the context holds is drawn by the list or by a display list it nests.
        let video_frame_ids = live_content
            .video_sinks
            .values()
            .map(|sink| (sink.id, sink.has_frame.then_some(sink.frame_id)))
            .collect();
        let image_frame_pixel_generations = live_content.image_frames.clone();
        self.painted_frame = Some(PaintedFrame {
            display_list: display_list.clone(),
            frame_inputs,
            viewport_size: IntSize {
                width: viewport_rect.width,
                height: viewport_rect.height,
            },
            canvas_content_generations,
            video_frame_ids,
            image_frame_pixel_generations,
        });
    }

    fn live_content_changes_since_last_paint(
        &self,
        viewport_size: IntSize,
        display_list: &DisplayList,
        live_content: &LiveContent,
    ) -> LiveContentChangesSinceLastPaint {
        let mut changes = LiveContentChangesSinceLastPaint::default();
        let Some(painted_frame) = &self.painted_frame else {
            changes.repaints_everything = true;
            return changes;
        };
        if painted_frame.viewport_size != viewport_size
            || painted_frame.display_list.surface_clear_color() != display_list.surface_clear_color()
        {
            changes.repaints_everything = true;
        }

        for (canvas_id, content_generation) in &painted_frame.canvas_content_generations {
            let current_content_generation = live_content
                .canvases
                .get(canvas_id)
                .map_or(0, |canvas| canvas.content_generation);
            if current_content_generation == *content_generation {
                continue;
            }
            changes.canvases.push(CanvasId(*canvas_id));
            changes.media_updates.push(MediaUpdate {
                media: LiveMedia::Canvas(CanvasId(*canvas_id)),
                content_version: current_content_generation,
            });
        }
        let summary = display_list.storage().summary();
        let image_frame_ids_of_list: HashSet<u64> = summary.image_frame_ids.iter().copied().collect();
        let video_sink_ids_of_list: HashSet<u64> = summary.video_sink_ids.iter().copied().collect();
        for (video_sink_id, frame_id) in &painted_frame.video_frame_ids {
            let current_frame_id = live_content
                .video_sinks
                .get(video_sink_id)
                .and_then(|sink| sink.has_frame.then_some(sink.frame_id));
            if current_frame_id == *frame_id {
                continue;
            }
            changes.media_updates.push(MediaUpdate {
                media: LiveMedia::VideoFrame(VideoSinkResourceId(*video_sink_id)),
                content_version: current_frame_id.unwrap_or(0),
            });
            // A video drawn only by a nested display list, such as an SVG image's, has no command in this list to
            // damage.
            if !video_sink_ids_of_list.contains(video_sink_id) {
                changes.repaints_everything = true;
            }
            changes.video_sinks.push(VideoSinkResourceId(*video_sink_id));
        }
        for (&image_frame_id, &pixel_generation) in &live_content.image_frames {
            if painted_frame.image_frame_pixel_generations.get(&image_frame_id) == Some(&pixel_generation) {
                continue;
            }
            // An image drawn only by a nested display list, such as an SVG image's, has no command in this list to
            // damage.
            if !image_frame_ids_of_list.contains(&image_frame_id) {
                changes.repaints_everything = true;
            }
            changes.image_frames.push(ImageFrameResourceId(image_frame_id));
        }
        changes
    }

    pub(super) fn frame_damage_for(
        &mut self,
        env: &PresentingEnvironment<'_>,
        pending_frame: &PendingFrame,
        frame: &FrameToPaint<'_>,
        live_content: &LiveContent,
    ) -> FrameDamage {
        let viewport_size = IntSize {
            width: pending_frame.viewport_rect.width,
            height: pending_frame.viewport_rect.height,
        };
        let viewport_rect = rect_of_size(viewport_size);
        let forced_damage_rect = pending_frame.forced_damage_rect.intersected(viewport_rect);
        let changes = self.live_content_changes_since_last_paint(viewport_size, frame.display_list, live_content);
        self.layers.media_activity.note_updates(&changes.media_updates, env.now);

        let mut layers = self.layer_update_for(env, frame, viewport_rect, live_content);
        let shown_by_window_server = match layers.plan(&self.layers) {
            Some(plan) => self.window_server_can_show_media_layers_of(env, plan, live_content),
            None => false,
        };
        layers.shown_by_window_server = shown_by_window_server;
        let layers_hold_last_frame = self.layers.has_plan() && self.painted_frame.is_some();
        let keeps_layer_rasters = match (layers.plan(&self.layers), self.layers.plan()) {
            (Some(plan), Some(last_plan)) => layers_hold_last_frame && plan.has_same_layers_as(last_plan),
            _ => false,
        };
        let repaints_everything = changes.repaints_everything || forced_damage_rect.contains_rect(viewport_rect);
        let damage = if repaints_everything {
            DisplayListDamage {
                rect: None,
                layer_rects: Vec::new(),
            }
        } else {
            self.damage_since_last_paint(
                viewport_rect,
                frame,
                forced_damage_rect,
                &changes,
                &layers,
                keeps_layer_rasters,
            )
        };
        layers.painted_damage = damage.rect.unwrap_or(viewport_rect).intersected(viewport_rect);

        let layer_count = layers.plan(&self.layers).map(|plan| plan.layers.len());
        match layer_count {
            None => {
                layers.repaints_everything =
                    layers_hold_last_frame || self.layers.repaint_everything_after_dropping_layers;
            }
            Some(_) if !keeps_layer_rasters => {
                layers.fully_repainted_layer_bits = u32::MAX;
                layers.repaints_everything = true;
            }
            Some(layer_count) => {
                for index in 0..layer_count {
                    let layer_damage = if damage.layer_rects.is_empty() {
                        Some(layers.painted_damage)
                    } else {
                        damage.layer_rects[index]
                    };
                    if layer_damage.is_none() {
                        layers.fully_repainted_layer_bits |= 1 << index;
                    }
                    layers
                        .damage_of_layers
                        .push(layer_damage.unwrap_or(viewport_rect).intersected(viewport_rect));
                }
            }
        }

        // A frame holds the canvases and videos only while the window server does not show them, so a switch either
        // way repaints all of it.
        if self.painted_frame.is_some() && layers.shown_by_window_server != self.layers.window_server_shows_media() {
            layers.repaints_everything = true;
            layers.fully_repainted_layer_bits = u32::MAX;
        }

        let live_media_changed = !changes.canvases.is_empty() || !changes.video_sinks.is_empty();
        if layers.repaints_everything {
            return FrameDamage {
                rect: viewport_rect,
                layers,
                live_media_changed,
            };
        }
        let mut damage_rect = layers.painted_damage;
        if let Some(plan) = layers.plan(&self.layers)
            && !layers.shown_by_window_server
        {
            damage_rect = damage_rect.united(
                super::layers::rect_of_media_layers_showing(plan, &changes.canvases, &changes.video_sinks)
                    .intersected(viewport_rect),
            );
        }
        FrameDamage {
            rect: damage_rect,
            layers,
            live_media_changed,
        }
    }

    fn damage_since_last_paint(
        &self,
        viewport_rect: IntRect,
        frame: &FrameToPaint<'_>,
        forced_damage_rect: IntRect,
        changes: &LiveContentChangesSinceLastPaint,
        layers: &LayerUpdate,
        keeps_layer_rasters: bool,
    ) -> DisplayListDamage {
        let painted_frame = self
            .painted_frame
            .as_ref()
            .expect("a frame that does not repaint everything has a frame painted before it");
        let plan = layers.plan(&self.layers);
        let media_record_offsets: Vec<u32> = plan.map_or_else(Vec::new, |plan| plan.media_record_offsets().collect());
        let layer_plans = match (plan, self.layers.plan()) {
            (Some(plan), Some(last_plan)) if keeps_layer_rasters => Some(LayerPlans {
                old: last_plan,
                new: plan,
            }),
            _ => None,
        };
        let mut damage = frame.frame_inputs.damage_since(
            frame.display_list.storage(),
            &painted_frame.frame_inputs,
            painted_frame.display_list.storage(),
            viewport_rect,
            LiveContentChanges {
                canvases: &changes.canvases,
                video_sinks: &changes.video_sinks,
                image_frames: &changes.image_frames,
                forced_damage_rect,
                offsets_of_records_composited_as_media_layers: &media_record_offsets,
            },
            layer_plans,
        );
        if let Some(rect) = damage.rect {
            damage.rect = Some(rect.intersected(viewport_rect));
        }
        damage
    }

    pub(super) fn layers_keep_plan_for(
        &self,
        frame: &FrameToPaint<'_>,
        viewport_rect: IntRect,
        active_media: &[LiveMedia],
    ) -> bool {
        let Some(painted_frame) = &self.painted_frame else {
            return false;
        };
        self.layers.has_plan()
            && Rc::ptr_eq(&painted_frame.display_list, frame.display_list)
            && painted_frame.viewport_size.width == viewport_rect.width
            && painted_frame.viewport_size.height == viewport_rect.height
            && self.layers.plan_active_media() == active_media
            && frame.frame_inputs.places_content_like(&painted_frame.frame_inputs)
    }
}
