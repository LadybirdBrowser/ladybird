/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a frame repaints of the frame painted before it, and of what each backing store holds.

use super::layers::{LayerUpdate, PaintedLayers, live_content_shown_by_frame};
use super::{ContextPresenting, FrameToPaint, PresentingEnvironment, rect_of_size};
use crate::frame_inputs::FrameInputs;
use crate::host::PendingFrame;
use crate::raster_host::LiveContent;
use libcompositing_rust::display_list::commands::CompositorContextId;
use libcompositing_rust::display_list::layers::{LayerContent, LayerIdentity};
use libcompositing_rust::display_list::summary::LiveContent as LiveMedia;
use libcompositing_rust::display_list::tile_keys::LiveContentShownByFrame;
use libcompositing_rust::scrolling::DisplayList;
use libgfx_rust::{IntRect, IntSize};
use std::collections::{HashMap, HashSet};
use std::rc::Rc;
use std::sync::Arc;

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

/// The layers a frame damages, where each holds a canvas or a video that the window server shows by itself, so that the
/// frame has nothing to paint.
pub struct MediaLayersDamagedAlone {
    media_of_damaged_layers: Vec<LiveMedia>,
    live_content_shown: LiveContentShownByFrame,
}

impl MediaLayersDamagedAlone {
    pub fn is_empty(&self) -> bool {
        self.media_of_damaged_layers.is_empty()
    }

    /// A canvas is drawn by the compositor, with GPU work the frame awaits. A video frame comes drawn.
    pub fn holds_canvas(&self) -> bool {
        self.media_of_damaged_layers
            .iter()
            .any(|media| matches!(media, LiveMedia::Canvas(_)))
    }
}

fn canvas_content_generations_of(display_list: &DisplayList, live_content: &LiveContent) -> HashMap<u64, u64> {
    let canvas_generation = |id: u64| {
        live_content
            .canvases
            .get(&id)
            .map_or(0, |canvas| canvas.content_generation)
    };
    display_list
        .storage()
        .summary()
        .canvas_ids
        .iter()
        .map(|&id| (id, canvas_generation(id)))
        .collect()
}

// The host reports the video sinks that the list draws.
fn video_frame_ids_of(live_content: &LiveContent) -> HashMap<u64, Option<u64>> {
    live_content
        .video_sinks
        .values()
        .map(|sink| (sink.handle, sink.has_frame.then_some(sink.frame_id)))
        .collect()
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
        let summary = display_list.storage().summary();
        let canvas_content_generations = canvas_content_generations_of(display_list, live_content);
        let video_frame_ids = video_frame_ids_of(live_content);
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

    /// Tells which layers a frame damages, where each holds a canvas or a video that the window server shows by itself:
    /// the layers of the painted frame, and what the window server shows of them, are those of such a frame too. None
    /// for a frame that may damage anything else, which has to be prepared in full.
    ///
    /// Such a frame draws the list of the painted frame as that one did, for the same window server, so the two differ
    /// at most in what they show of live content, and a layer is damaged where its record shows something else. The
    /// window server can show now the canvases and videos it showed then and no other: one it could show and the frame
    /// still held makes every frame be prepared in full.
    pub(super) fn media_layers_damaged_alone_since_painted_frame(
        &self,
        env: &PresentingEnvironment<'_>,
        pending_frame: &PendingFrame,
        frame: &FrameToPaint<'_>,
        live_content: &LiveContent,
    ) -> Option<MediaLayersDamagedAlone> {
        let painted_frame = self.painted_frame.as_ref()?;
        let painted_layers = &painted_frame.layers;
        let display_list = frame.display_list;
        let viewport_size = IntSize {
            width: pending_frame.viewport_rect.width,
            height: pending_frame.viewport_rect.height,
        };
        // A frame draws with the stack of its plan within the viewport the plan was made for.
        let draws_with_stack_of_painted_frame = frame.frame_inputs.layer_plan.as_ref().is_some_and(|layer_plan| {
            Rc::ptr_eq(&layer_plan.stack, &painted_layers.stack) && layer_plan.viewport_rect == self.raster_rect()
        });
        let frame_inputs = frame.frame_inputs;
        let inputs_of_painted_frame = &painted_frame.frame_inputs;
        let draws_list_as_painted_frame = self.presented_frame == Some(pending_frame.viewport_rect)
            && pending_frame.forced_damage_rect.is_empty()
            && painted_frame.viewport_size == viewport_size
            && Rc::ptr_eq(&painted_frame.display_list, display_list)
            && draws_with_stack_of_painted_frame
            && Arc::ptr_eq(
                frame_inputs.visual_context_tree.as_arc(),
                inputs_of_painted_frame.visual_context_tree.as_arc(),
            )
            && frame_inputs
                .presented_scroll_state_snapshot
                .holds_the_offsets_of(&inputs_of_painted_frame.presented_scroll_state_snapshot)
            && painted_frame.allows_lcd_text == self.allows_lcd_text(display_list, frame_inputs)
            // The overlays draw the same but for that of the compositor's layers, which the frame is to have none of.
            && frame_inputs.overlays.draws_same_as(&inputs_of_painted_frame.overlays)
            && self.layer_overlay.overlay().is_none()
            && painted_layers.window_server_shows_every_layer()
                == self.latest_frame_was_presented_without_store_below_sublayers
            && painted_layers.window_server_compositing == self.window_server_compositing(env)
            // An image that only a filter of the tree draws is live content of no record of the list.
            && live_content.image_frames == painted_frame.image_frame_pixel_generations;
        if !draws_list_as_painted_frame {
            return None;
        }
        let live_content_shown = live_content_shown_by_frame(env, frame, live_content, &painted_layers.stack);
        let shown_by_painted_frame = &painted_layers.live_content_shown;
        if live_content_shown.in_bands != shown_by_painted_frame.in_bands {
            return None;
        }
        let window_server_may_show_media = self.why_window_server_shows_no_media_of_this_context(env).is_none();
        let mut media_of_damaged_layers = Vec::new();
        for compositing in &painted_layers.compositing_of_layers {
            let layer_index = compositing.layer_index;
            let layer = compositing.layer_in(&painted_layers.stack);
            let layer_is_damaged = live_content_shown.in_layers_of_their_own[layer_index.0]
                != shown_by_painted_frame.in_layers_of_their_own[layer_index.0];
            let (LayerContent::Media { .. }, LayerIdentity::Media { content, .. }) = (&layer.content, layer.identity)
            else {
                // The compositor paints a caret or a scroll bar that has a layer of its own.
                if layer_is_damaged {
                    return None;
                }
                continue;
            };
            let window_server_can_show_layer = window_server_may_show_media
                && self
                    .why_window_server_does_not_show_media_layer(
                        env,
                        &painted_layers.window_server_placements_of_media,
                        layer_index,
                        layer,
                        live_content,
                    )
                    .is_none();
            if window_server_can_show_layer != compositing.shown_by_window_server {
                return None;
            }
            if !layer_is_damaged {
                continue;
            }
            // The frame holds what it replays of a canvas or a video.
            if !compositing.shown_by_window_server {
                return None;
            }
            media_of_damaged_layers.push(content);
        }
        Some(MediaLayersDamagedAlone {
            media_of_damaged_layers,
            live_content_shown,
        })
    }

    /// Remembers what a frame shows of live content, where it damages only layers that hold media the window server
    /// shows by itself.
    pub(super) fn remember_frame_that_damages_media_layers_alone(
        &mut self,
        live_content: &LiveContent,
        damaged_layers: MediaLayersDamagedAlone,
    ) {
        let Some(painted_frame) = &mut self.painted_frame else {
            return;
        };
        painted_frame.canvas_content_generations =
            canvas_content_generations_of(&painted_frame.display_list, live_content);
        painted_frame.video_frame_ids = video_frame_ids_of(live_content);
        painted_frame.layers.live_content_shown = damaged_layers.live_content_shown;
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

        let scrolled_compositor_scrollbars = self.painted_frame.as_ref().map_or(IntRect::default(), |painted_frame| {
            frame
                .frame_inputs
                .gutters_of_compositor_painted_scrollbars_that_scrolled_since(&painted_frame.frame_inputs)
        });
        let forced_damage_rect =
            self.raster_rect_of(forced_damage_rect.united(scrolled_compositor_scrollbars.intersected(viewport_rect)));
        let damage_of_layers_moved_by_animated_transforms =
            self.damage_of_layers_moved_by_animated_transforms(&layer_update, raster_rect);
        let repaints_everything = invalidates_every_tile
            || damage_of_layers_moved_by_animated_transforms.is_none()
            || self.layer_overlay.repaints_everything();
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
