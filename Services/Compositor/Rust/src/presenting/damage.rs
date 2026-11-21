/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a frame repaints of the frame painted before it, and of what each backing store holds.

use super::layers::{LayerUpdate, PaintedLayers};
use super::{ContextPresenting, FrameToPaint, PresentingEnvironment, rect_of_size};
use crate::frame_inputs::FrameInputs;
use crate::host::PendingFrame;
use crate::raster_host::LiveContent;
use libcompositing_rust::display_list::commands::CompositorContextId;
use libcompositing_rust::scrolling::DisplayList;
use libgfx_rust::{IntRect, IntSize};
use std::collections::{HashMap, HashSet};
use std::rc::Rc;

/// The frame painted last: its display list and inputs, what it did with its layers, and the version of each live
/// content it drew, which comparing lists cannot see change.
pub struct PaintedFrame {
    pub display_list: Rc<DisplayList>,
    pub frame_inputs: FrameInputs,
    pub layers: PaintedLayers,
    pub viewport_size: IntSize,
    allows_lcd_text: bool,
    canvas_content_generations: HashMap<u64, u64>,
    video_frame_ids: HashMap<u64, Option<u64>>,
    image_frame_pixel_generations: HashMap<u64, u64>,
    nested_context_frame_generations: HashMap<u64, Option<u64>>,
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
    missed_damage_by_store_least_recently_painted_first: Vec<(i32, IntRect)>,
}

// Twice as many as a context ever has at once.
const MAXIMUM_REMEMBERED_STORE_COUNT: usize = 16;

impl StoreDamage {
    pub fn forget_every_store(&mut self) {
        self.missed_damage_by_store_least_recently_painted_first.clear();
    }

    /// What painting a frame into the store repaints: the frame's damage, and what the store missed since it was last
    /// painted, all of it for a store never painted into.
    pub fn take(&mut self, bitmap_id: i32, frame_damage: IntRect, store_rect: IntRect) -> IntRect {
        let mut repaint_rect = store_rect;
        for (store_bitmap_id, missed_damage) in &mut self.missed_damage_by_store_least_recently_painted_first {
            if *store_bitmap_id == bitmap_id {
                repaint_rect = missed_damage.united(frame_damage);
            }
            *missed_damage = missed_damage.united(frame_damage);
        }
        self.missed_damage_by_store_least_recently_painted_first
            .retain(|(store_bitmap_id, _)| *store_bitmap_id != bitmap_id);
        self.missed_damage_by_store_least_recently_painted_first
            .push((bitmap_id, IntRect::default()));
        if self.missed_damage_by_store_least_recently_painted_first.len() > MAXIMUM_REMEMBERED_STORE_COUNT {
            self.missed_damage_by_store_least_recently_painted_first.remove(0);
        }
        repaint_rect
    }

    /// Damage discovered while painting the current store is still owed to the other stores.
    pub fn add_damage_to_other_stores(&mut self, bitmap_id: i32, damage: IntRect) {
        for (store_bitmap_id, missed_damage) in &mut self.missed_damage_by_store_least_recently_painted_first {
            if *store_bitmap_id != bitmap_id {
                *missed_damage = missed_damage.united(damage);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_late_full_repaint_is_owed_to_the_other_stores() {
        let mut damage = StoreDamage::default();
        let full_rect = IntRect::new(0, 0, 100, 100);
        let partial_rect = IntRect::new(10, 10, 20, 20);
        damage.take(1, full_rect, full_rect);
        damage.take(2, full_rect, full_rect);
        assert_eq!(damage.take(1, partial_rect, full_rect), full_rect);
        assert_eq!(damage.take(2, partial_rect, full_rect), partial_rect);

        damage.add_damage_to_other_stores(2, full_rect);
        assert_eq!(damage.take(2, partial_rect, full_rect), partial_rect);
        assert_eq!(damage.take(1, partial_rect, full_rect), full_rect);
    }
}

#[derive(Default)]
pub struct LiveContentChangesSinceLastPaint {
    pub canvas_or_video_changed: bool,
    pub repaints_everything: bool,
}

pub struct FrameDamage {
    pub rect_in_raster_device_pixels: IntRect,
    pub layers: LayerUpdate,
    pub live_media_changed: bool,
}

impl ContextPresenting {
    /// The live content of the context as it is now, with the canvases that the list and the frame painted last draw.
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

    /// Remembers a frame that is painted, or that shows nothing new: what it draws, and what it did with its layers.
    pub(super) fn remember_painted_frame(
        &mut self,
        env: &PresentingEnvironment<'_>,
        viewport_rect: IntRect,
        display_list: &Rc<DisplayList>,
        frame_inputs: FrameInputs,
        live_content: &LiveContent,
        layer_update: LayerUpdate,
    ) {
        let allows_lcd_text = layer_update.frame_allows_lcd_text;
        let layers = self.commit_layer_update(env, layer_update);
        let canvas_generation = |id: u64| {
            live_content
                .canvases
                .get(&id)
                .map_or(0, |canvas| canvas.content_generation)
        };
        let summary = display_list.storage().summary();
        let canvas_content_generations = summary
            .canvas_ids
            .iter()
            .map(|&id| (id, canvas_generation(id)))
            .collect();
        // The host reports the video sinks that the list draws.
        let video_frame_ids = live_content
            .video_sinks
            .values()
            .map(|sink| (sink.handle, sink.has_frame.then_some(sink.frame_id)))
            .collect();
        let image_frame_pixel_generations = live_content.image_frames.clone();
        let nested_context_frame_generations = summary
            .composited_context_ids
            .iter()
            .map(|&id| (id, env.nested_contexts.frame_generation_of(CompositorContextId(id))))
            .collect();
        self.painted_frame = Some(PaintedFrame {
            display_list: display_list.clone(),
            frame_inputs,
            layers,
            viewport_size: IntSize {
                width: viewport_rect.width,
                height: viewport_rect.height,
            },
            allows_lcd_text,
            canvas_content_generations,
            video_frame_ids,
            image_frame_pixel_generations,
            nested_context_frame_generations,
        });
    }

    fn live_content_changes_since_last_paint(
        &self,
        env: &PresentingEnvironment<'_>,
        viewport_size: IntSize,
        display_list: &DisplayList,
        allows_lcd_text: bool,
        live_content: &LiveContent,
    ) -> LiveContentChangesSinceLastPaint {
        let mut changes = LiveContentChangesSinceLastPaint::default();
        let Some(painted_frame) = &self.painted_frame else {
            changes.repaints_everything = true;
            return changes;
        };
        if painted_frame.viewport_size != viewport_size
            || painted_frame.display_list.surface_clear_color() != display_list.surface_clear_color()
            || painted_frame.allows_lcd_text != allows_lcd_text
        {
            changes.repaints_everything = true;
        }

        // Live content that no command of this list draws, such as an image that a filter of the visual context tree
        // draws, has no command in this list to damage.
        let canvas_ids_of_list: HashSet<u64> = display_list.storage().summary().canvas_ids.iter().copied().collect();
        for (canvas_id, content_generation) in &painted_frame.canvas_content_generations {
            let current_content_generation = live_content
                .canvases
                .get(canvas_id)
                .map_or(0, |canvas| canvas.content_generation);
            if current_content_generation == *content_generation {
                continue;
            }
            if !canvas_ids_of_list.contains(canvas_id) {
                changes.repaints_everything = true;
            }
            changes.canvas_or_video_changed = true;
        }
        let summary = display_list.storage().summary();
        let image_frame_ids_of_list: HashSet<u64> = summary.image_frame_ids.iter().copied().collect();
        let video_sink_handles_of_list: HashSet<u64> = summary.video_sink_handles.iter().copied().collect();
        for (video_sink_handle, frame_id) in &painted_frame.video_frame_ids {
            let current_frame_id = live_content
                .video_sinks
                .get(video_sink_handle)
                .and_then(|sink| sink.has_frame.then_some(sink.frame_id));
            if current_frame_id == *frame_id {
                continue;
            }
            if !video_sink_handles_of_list.contains(video_sink_handle) {
                changes.repaints_everything = true;
            }
            changes.canvas_or_video_changed = true;
        }
        for (&image_frame_id, &pixel_generation) in &live_content.image_frames {
            if painted_frame.image_frame_pixel_generations.get(&image_frame_id) == Some(&pixel_generation) {
                continue;
            }
            if !image_frame_ids_of_list.contains(&image_frame_id) {
                changes.repaints_everything = true;
            }
        }
        let nested_context_ids_of_list: HashSet<u64> = summary.composited_context_ids.iter().copied().collect();
        for (&nested_context_id, &frame_generation) in &painted_frame.nested_context_frame_generations {
            let nested_context = CompositorContextId(nested_context_id);
            if env.nested_contexts.frame_generation_of(nested_context) == frame_generation {
                continue;
            }
            if !nested_context_ids_of_list.contains(&nested_context_id) {
                changes.repaints_everything = true;
            }
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
        let raster_rect = self.raster_rect();
        let forced_damage_rect = pending_frame.forced_damage_rect.intersected(viewport_rect);
        let allows_lcd_text = self.allows_lcd_text(frame.display_list, frame.frame_inputs);
        let changes = self.live_content_changes_since_last_paint(
            env,
            viewport_size,
            frame.display_list,
            allows_lcd_text,
            live_content,
        );
        let invalidates_every_tile = changes.repaints_everything;
        let (layer_update, stale_rect) = self.layer_update_for(
            env,
            frame,
            raster_rect,
            live_content,
            invalidates_every_tile,
            allows_lcd_text,
        );
        let live_media_changed = changes.canvas_or_video_changed;

        let forced_damage_rect = self.raster_rect_of(forced_damage_rect);
        let damage_of_layers_moved_by_animated_transforms =
            self.damage_of_layers_moved_by_animated_transforms(&layer_update, raster_rect);
        // Every pixel of a frame that maps the raster to its pixels through a transform, or of the frame after one, can
        // show another part of the raster than the frame before showed there.
        let frame_or_frame_before_maps_raster_through_transform = frame.frame_inputs.raster_to_frame.is_some()
            || self
                .painted_frame
                .as_ref()
                .is_some_and(|painted_frame| painted_frame.frame_inputs.raster_to_frame.is_some());
        let repaints_everything = invalidates_every_tile
            || damage_of_layers_moved_by_animated_transforms.is_none()
            || self.layer_overlay.repaints_everything()
            || frame_or_frame_before_maps_raster_through_transform;
        let stale_rect = stale_rect.united(damage_of_layers_moved_by_animated_transforms.unwrap_or_default());
        FrameDamage {
            rect_in_raster_device_pixels: if repaints_everything {
                raster_rect
            } else {
                layer_update.damage_grown_over_what_composited_effects_read(forced_damage_rect.united(stale_rect))
            },
            layers: layer_update,
            live_media_changed,
        }
    }
}
