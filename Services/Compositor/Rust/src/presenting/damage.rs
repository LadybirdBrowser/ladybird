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
use libcompositing_rust::scrolling::DisplayList;
use libgfx_rust::{IntRect, IntSize};
use std::rc::Rc;

/// The frame painted last: its display list and inputs, and what it did with its layers.
pub struct PaintedFrame {
    pub display_list: Rc<DisplayList>,
    pub frame_inputs: FrameInputs,
    pub layers: PaintedLayers,
    pub viewport_size: IntSize,
    allows_lcd_text: bool,
}

impl PaintedFrame {
    pub fn draws_canvas(&self, canvas_id: u64) -> bool {
        self.display_list
            .storage()
            .summary()
            .canvas_ids
            .binary_search(&canvas_id)
            .is_ok()
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

pub struct FrameDamage {
    pub rect_in_raster_device_pixels: IntRect,
    pub layers: LayerUpdate,
}

impl ContextPresenting {
    /// The live content of the context as it is now, with the canvases that the list draws.
    pub(super) fn live_content(&self, env: &PresentingEnvironment<'_>, display_list: &DisplayList) -> LiveContent {
        env.raster
            .live_content(env.context_id, &display_list.storage().summary().canvas_ids)
    }

    /// Remembers a frame that is painted, or that shows nothing new: what it draws, and what it did with its layers.
    pub(super) fn remember_painted_frame(
        &mut self,
        env: &PresentingEnvironment<'_>,
        viewport_rect: IntRect,
        display_list: &Rc<DisplayList>,
        frame_inputs: FrameInputs,
        layer_update: LayerUpdate,
    ) {
        let allows_lcd_text = layer_update.frame_allows_lcd_text;
        let layers = self.commit_layer_update(env, layer_update);
        self.painted_frame = Some(PaintedFrame {
            display_list: display_list.clone(),
            frame_inputs,
            layers,
            viewport_size: IntSize {
                width: viewport_rect.width,
                height: viewport_rect.height,
            },
            allows_lcd_text,
        });
    }

    /// Whether the frame repaints the store in full, as it is to be presented in full, no frame was painted since the
    /// stores were made, or the size of the viewport, the clear color or whether text may be drawn with LCD
    /// antialiasing differs from the frame painted last.
    fn repaints_store_in_full(
        &self,
        viewport_size: IntSize,
        display_list: &DisplayList,
        allows_lcd_text: bool,
    ) -> bool {
        if self.presents_next_frame_in_full {
            return true;
        }
        let Some(painted_frame) = &self.painted_frame else {
            return true;
        };
        painted_frame.viewport_size != viewport_size
            || painted_frame.display_list.surface_clear_color() != display_list.surface_clear_color()
            || painted_frame.allows_lcd_text != allows_lcd_text
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
        let allows_lcd_text = self.allows_lcd_text(frame.display_list, frame.frame_inputs);
        let repaints_store_in_full = self.repaints_store_in_full(viewport_size, frame.display_list, allows_lcd_text);
        let (layer_update, stale_rect) = self.layer_update_for(env, frame, raster_rect, live_content, allows_lcd_text);

        let overlay_damage_rect = self
            .painted_frame
            .as_ref()
            .map_or_else(IntRect::default, |painted_frame| {
                frame
                    .frame_inputs
                    .overlays
                    .damage_since(&painted_frame.frame_inputs.overlays)
            });
        let overlay_damage_rect = self.raster_rect_of(overlay_damage_rect.intersected(viewport_rect));
        let damage_of_layers_moved_by_animated_transforms =
            self.damage_of_layers_moved_by_animated_transforms(&layer_update, raster_rect);
        // Every pixel of a frame that maps the raster to its pixels through a transform, or of the frame after one, can
        // show another part of the raster than the frame before showed there.
        let frame_or_frame_before_maps_raster_through_transform = frame.frame_inputs.raster_to_frame.is_some()
            || self
                .painted_frame
                .as_ref()
                .is_some_and(|painted_frame| painted_frame.frame_inputs.raster_to_frame.is_some());
        let repaints_everything = repaints_store_in_full
            || damage_of_layers_moved_by_animated_transforms.is_none()
            || self.layer_overlay.repaints_everything()
            || frame_or_frame_before_maps_raster_through_transform;
        let stale_rect = stale_rect.united(damage_of_layers_moved_by_animated_transforms.unwrap_or_default());
        FrameDamage {
            rect_in_raster_device_pixels: if repaints_everything {
                raster_rect
            } else {
                layer_update.damage_grown_over_what_composited_effects_read(overlay_damage_rect.united(stale_rect))
            },
            layers: layer_update,
        }
    }
}
