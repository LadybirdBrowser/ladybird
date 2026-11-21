/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Works out what a tile holds over a part of its cell from its records, without rastering it.

use super::{
    BandBoundToListAndGeometry, BandTileKeys, KeyedTile, TileKeyingOfList, TileKeyingOfRasterSpace, TileReplayItem,
};
use crate::display_list::commands::{ClipMode, ClipNodeIndex, ContextRef, EffectNodeIndex};
use crate::display_list::effect_geometry::nearest_effects_at_or_above_where;
use crate::display_list::window_server_placement::RoundedRect;
use crate::visual_context::{ClipData, ClipNode, ClipNodeData, EffectNodeData};
use libgfx_rust::{Color, CompositingAndBlendingOperator, FloatRect, IntRect};
use std::collections::HashSet;

/// What a tile holds over a part of its cell, as its records tell without rastering it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TileCoverage {
    /// Every pixel of one color, which a raster gives exactly: the clear color of its layer, or an opaque one.
    OneColor(Color),
    /// Every pixel opaque, of more than one color.
    Opaque,
    Unknown,
}

impl TileCoverage {
    pub fn is_opaque(self) -> bool {
        match self {
            TileCoverage::OneColor(color) => color.alpha() == 255,
            TileCoverage::Opaque => true,
            TileCoverage::Unknown => false,
        }
    }

    fn under_plain_fill_of(self, color: Color) -> Self {
        match color.alpha() {
            255 => TileCoverage::OneColor(color),
            0 => self,
            _ if self.is_opaque() => TileCoverage::Opaque,
            _ => TileCoverage::Unknown,
        }
    }

    fn under_content_other_than_plain_fill(self, content_can_lower_destination_alpha: bool) -> Self {
        if self.is_opaque() && !content_can_lower_destination_alpha {
            TileCoverage::Opaque
        } else {
            TileCoverage::Unknown
        }
    }
}

/// The clips, and the effects whose output clips, a walk for the coverage of a tile found to leave its area unclipped,
/// so that walks of chains that share nodes stop where they meet.
#[derive(Default)]
struct WhatLeavesAreaOfTileUnclipped {
    clips: HashSet<ClipNodeIndex>,
    effects: HashSet<EffectNodeIndex>,
}

enum WhatOneFillDoesWithinArea {
    DrawsNothing,
    CoversItWith(Color),
}

impl RoundedRect {
    fn covers(&self, area: FloatRect) -> bool {
        self.rect.contains_rect(area) && !self.rounded_corner_reaches_into(area)
    }

    fn misses(&self, area: FloatRect) -> bool {
        self.rect.intersected(area).is_empty()
    }
}

impl TileKeyingOfList<'_> {
    /// Whether the effects that tiles apply at and around `effect` are plain groups.
    fn effects_applied_in_tiles_are_plain_groups(&self, effect: EffectNodeIndex) -> bool {
        if effect.is_none() {
            return true;
        }
        let nearest = self
            .nearest_composited_or_not_plain_effect_by_effect_index
            .get_or_init(|| {
                nearest_effects_at_or_above_where(self.tree, |effect, node| {
                    self.compositor_composites(effect)
                        || !matches!(&node.data, EffectNodeData::Effects(effects)
                            if effects.opacity == 1.0
                                && effects.filter.is_none()
                                && effects.backdrop_filter.is_none()
                                && effects.blend_mode == CompositingAndBlendingOperator::Normal)
                })
            })[effect.0 as usize];
        nearest.is_none() || self.compositor_composites(nearest)
    }

    /// Whether an effect that tiles apply at or around `effect` blends in a way that can lower the alpha below it.
    fn effects_applied_in_tiles_can_lower_destination_alpha(&self, effect: EffectNodeIndex) -> bool {
        if effect.is_none() {
            return false;
        }
        let nearest = self
            .nearest_composited_or_alpha_lowering_effect_by_effect_index
            .get_or_init(|| {
                nearest_effects_at_or_above_where(self.tree, |effect, node| {
                    self.compositor_composites(effect)
                        || matches!(&node.data, EffectNodeData::Effects(effects)
                            if effects.blend_mode.lowers_destination_alpha())
                })
            })[effect.0 as usize];
        !nearest.is_none() && !self.compositor_composites(nearest)
    }
}

impl TileKeyingOfRasterSpace<'_> {
    fn effect_chain_applied_in_tiles(&self, effect: EffectNodeIndex) -> impl Iterator<Item = EffectNodeIndex> {
        let tree = self.list.tree;
        std::iter::successors((!effect.is_none()).then_some(effect), move |effect| {
            Some(tree.effect_nodes[effect.0 as usize].parent).filter(|parent| !parent.is_none())
        })
        .take_while(|&effect| !self.list.compositor_composites(effect))
    }

    fn clip_node_leaves_area_unclipped(&self, node: &ClipNode, area_in_device_pixels: FloatRect) -> bool {
        let ClipNodeData::Rect(ClipData {
            rect,
            corner_radii,
            mode: ClipMode::Intersect,
        }) = &node.data
        else {
            return false;
        };
        !node.clips_everything
            && RoundedRect::device_aligned_of(&self.geometry.world, node.spatial, *rect, corner_radii)
                .is_some_and(|rect| rect.covers(area_in_device_pixels))
    }

    fn clip_chain_leaves_area_unclipped(
        &self,
        clip: ClipNodeIndex,
        area_in_device_pixels: FloatRect,
        known: &mut WhatLeavesAreaOfTileUnclipped,
    ) -> bool {
        let tree = self.list.tree;
        let mut clips_walked = Vec::new();
        let mut clip = clip;
        while !clip.is_none() && !known.clips.contains(&clip) {
            let node = &tree.clip_nodes[clip.0 as usize];
            if !self.geometry.clip_is_left_to_composite(clip)
                && !self.clip_node_leaves_area_unclipped(node, area_in_device_pixels)
            {
                return false;
            }
            clips_walked.push(clip);
            clip = node.parent;
        }
        known.clips.extend(clips_walked);
        true
    }

    fn output_clips_of_effects_applied_in_tiles_leave_area_unclipped(
        &self,
        effect: EffectNodeIndex,
        area_in_device_pixels: FloatRect,
        known: &mut WhatLeavesAreaOfTileUnclipped,
    ) -> bool {
        let mut effects_walked = Vec::new();
        for effect in self.effect_chain_applied_in_tiles(effect) {
            if known.effects.contains(&effect) {
                break;
            }
            if !self.clip_chain_leaves_area_unclipped(
                self.list.effect_clips.output_clip(effect),
                area_in_device_pixels,
                known,
            ) {
                return false;
            }
            effects_walked.push(effect);
        }
        known.effects.extend(effects_walked);
        true
    }

    fn context_applies_no_clip_or_effect_within(
        &self,
        context: ContextRef,
        area_in_device_pixels: FloatRect,
        known: &mut WhatLeavesAreaOfTileUnclipped,
    ) -> bool {
        let backface_culled = self
            .geometry
            .world
            .backface_culled
            .get(context.spatial.0 as usize)
            .copied()
            .unwrap_or(false);
        !backface_culled
            && self.list.effects_applied_in_tiles_are_plain_groups(context.effect)
            && self.clip_chain_leaves_area_unclipped(context.clip, area_in_device_pixels, known)
            && self.output_clips_of_effects_applied_in_tiles_leave_area_unclipped(
                context.effect,
                area_in_device_pixels,
                known,
            )
    }

    fn what_one_fill_item_does_within_area(
        &self,
        bound_band: &BandBoundToListAndGeometry,
        item: TileReplayItem,
        area_in_device_pixels: FloatRect,
        known: &mut WhatLeavesAreaOfTileUnclipped,
    ) -> Option<WhatOneFillDoesWithinArea> {
        let TileReplayItem::PlacedRecordRange {
            chunk_index,
            range_index,
        } = item
        else {
            return None;
        };
        let range = bound_band.placed_record_range(chunk_index, range_index);
        let fill = range.device_aligned_fill.as_ref()?;
        // The inner rect's corners curve only within the outer rect's corners.
        let area_is_inside_unfilled_inner_rect = fill.rect.covers(area_in_device_pixels)
            && fill
                .unfilled_inner_rect_of_ring
                .is_some_and(|unfilled_inner_rect| unfilled_inner_rect.covers(area_in_device_pixels));
        if (fill.rect.misses(area_in_device_pixels) || area_is_inside_unfilled_inner_rect)
            && range.is_spread_by_no_filter
        {
            return Some(WhatOneFillDoesWithinArea::DrawsNothing);
        }
        let fill_covers_area = fill.rect.covers(area_in_device_pixels)
            && fill
                .unfilled_inner_rect_of_ring
                .is_none_or(|unfilled_inner_rect| unfilled_inner_rect.misses(area_in_device_pixels));
        (fill_covers_area
            && fill.draws_recorded_color_with_source_over
            && self.context_applies_no_clip_or_effect_within(range.context, area_in_device_pixels, known))
        .then_some(WhatOneFillDoesWithinArea::CoversItWith(fill.color))
    }

    fn item_can_lower_destination_alpha(&self, bound_band: &BandBoundToListAndGeometry, item: TileReplayItem) -> bool {
        let chunk = self.list.display_list.chunk(item.chunk_index());
        self.records_of_item(bound_band, item).iter().any(|record| {
            let context = chunk.runs()[record.run_index.0 as usize].context;
            record.can_lower_destination_alpha
                || self
                    .list
                    .effects_applied_in_tiles_can_lower_destination_alpha(context.effect)
        })
    }

    /// What a raster of `tile` holds over `area` of its cell, in the device pixels of the space, as its replay items
    /// tell in paint order from `clear_color`, the color its layer's surface starts as.
    pub fn coverage_of_tile(
        &self,
        band_tile_keys: &BandTileKeys,
        tile: &KeyedTile,
        area: IntRect,
        clear_color: Color,
    ) -> TileCoverage {
        let bound_band = &band_tile_keys.bound_band;
        if self.list.replays_every_record_into_every_tile || area.is_empty() {
            return TileCoverage::Unknown;
        }
        // Fills are walked in list order, which depth-sorted planes are not replayed in.
        if self.list.tree_sorts_planes_by_depth
            && tile
                .replay_items_in_paint_order
                .iter()
                .any(|&item| self.root_of_context_sorting_item_by_depth(bound_band, item).is_some())
        {
            return TileCoverage::Unknown;
        }
        let area_in_device_pixels = area.to_float();
        let mut known_to_leave_area_unclipped = WhatLeavesAreaOfTileUnclipped::default();
        let mut coverage = TileCoverage::OneColor(clear_color);
        for &item in &tile.replay_items_in_paint_order {
            if matches!(item, TileReplayItem::PlacedRecordRange { .. })
                && bound_band.reach_of_item(item).part_of(area).is_empty()
            {
                continue;
            }
            coverage = match self.what_one_fill_item_does_within_area(
                bound_band,
                item,
                area_in_device_pixels,
                &mut known_to_leave_area_unclipped,
            ) {
                Some(WhatOneFillDoesWithinArea::DrawsNothing) => coverage,
                Some(WhatOneFillDoesWithinArea::CoversItWith(color)) => coverage.under_plain_fill_of(color),
                None => coverage
                    .under_content_other_than_plain_fill(self.item_can_lower_destination_alpha(bound_band, item)),
            };
            if coverage == TileCoverage::Unknown {
                break;
            }
        }
        coverage
    }
}
