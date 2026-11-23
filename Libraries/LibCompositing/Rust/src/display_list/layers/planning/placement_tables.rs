/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What placing a list reads of the raster spaces at a set of anchors: the clips that rasters of each visual context
//! leave to apply when they are composited, the effects that keep content out of a space, and the runs whose records
//! reach outside their visual context.

use super::raster_spaces_for_placing::{
    RasterSpaceGeometryForPlacing, RasterSpacesForPlacing, rect_swept_over_translations,
};
use crate::display_list::animated_transform_spaces::spatial_nodes_under_transform_animations;
use crate::display_list::chunks::ReachOutsideRunContext;
use crate::display_list::commands::{ClipMode, ContextRef, DisplayListCommandRun, EffectNodeIndex, SpatialNodeIndex};
use crate::display_list::effect_geometry::backdrop_filter_read_reach;
use crate::display_list::placement::{
    CompositeClip, EXTENT_LIMIT_IN_DEVICE_PIXELS, RasterSpace, RasterSpaceAnchorKind, RasterSpaceIndex,
    space_and_ancestors_below_viewport, space_is_ancestor_or_self, translation_if_only_translates,
};
use crate::display_list::reach::Reach;
use crate::display_list::storage::{ChunkIndexInList, DisplayListStorage};
use crate::visual_context::{ClipNodeData, EffectNodeData, VisualContextTree};
use libgfx_rust::IntRect;
use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;

const COMPOSITE_CLIP_EDGE_TOLERANCE_IN_DEVICE_PIXELS: f32 = 1.0 / 64.0;

impl CompositeClip {
    /// Whether the clip leaves all of `rect` unclipped: it lies within the clip's rect and outside its corner boxes.
    fn leaves_all_of(&self, rect: IntRect) -> bool {
        if !self.rect.contains_rect(rect) {
            return false;
        }
        let radii = self.corner_radii;
        let (left, top, right, bottom) = (self.rect.x, self.rect.y, self.rect.right(), self.rect.bottom());
        [
            IntRect::new(
                left,
                top,
                radii.top_left.horizontal_radius,
                radii.top_left.vertical_radius,
            ),
            IntRect::new(
                right - radii.top_right.horizontal_radius,
                top,
                radii.top_right.horizontal_radius,
                radii.top_right.vertical_radius,
            ),
            IntRect::new(
                right - radii.bottom_right.horizontal_radius,
                bottom - radii.bottom_right.vertical_radius,
                radii.bottom_right.horizontal_radius,
                radii.bottom_right.vertical_radius,
            ),
            IntRect::new(
                left,
                bottom - radii.bottom_left.vertical_radius,
                radii.bottom_left.horizontal_radius,
                radii.bottom_left.vertical_radius,
            ),
        ]
        .iter()
        .all(|corner_box| corner_box.is_empty() || corner_box.intersected(rect).is_empty())
    }
}

pub(super) fn lowest_common_ancestor_space(
    spaces: &[RasterSpace],
    first: RasterSpaceIndex,
    second: RasterSpaceIndex,
) -> RasterSpaceIndex {
    let mut candidate = first;
    while !space_is_ancestor_or_self(spaces, candidate, second) {
        candidate = spaces[candidate.0].parent_index;
    }
    candidate
}

type CompositeClipsOfContextInSpace = Option<Rc<[CompositeClip]>>;

/// Where content is placed among the spaces at a set of anchors: the spaces, the clips that can be left to apply when
/// rasters are composited, and the effects whose content reads its backdrop. Where each context is placed is worked out
/// as it is first asked for.
pub(super) struct PlacementTables {
    spaces: RasterSpacesForPlacing,
    /// Per spatial node, whether a sample of the compositor's animations can move it within the deepest space it is
    /// in, as it is under a node whose transform the compositor animates that anchors no space. Empty where the tree
    /// has no transform animations.
    spatial_node_moves_with_animation_samples: Vec<bool>,
    composite_form_of_clips: Vec<Option<CompositeClip>>,
    placement_constraint_of_effects: Vec<EffectPlacementConstraint>,
    deepest_space_and_backdrop_filter_read_rect_by_effect: Vec<Option<(RasterSpaceIndex, IntRect)>>,
    animated_space_holding_every_run_of_effect: Vec<Option<RasterSpaceIndex>>,
    refused_animated_spaces: RefCell<Vec<bool>>,
    /// Per effect node, whether the compositor composites it around content it keeps in several spaces, so that it
    /// neither keeps its content out of spaces deeper than its own nor moves that content within a space. Empty where
    /// no effect is.
    effect_is_composited_around_content_of_several_spaces_by_effect_index: Vec<bool>,
    cached_composite_clips_of_contexts:
        RefCell<HashMap<(ContextRef, RasterSpaceIndex), CompositeClipsOfContextInSpace>>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum EffectPlacementConstraint {
    ConstrainsNothing,
    MustMoveWithSpace,
    MustMoveWithSpaceAndKeepBackdropReadUncut,
}

impl PlacementTables {
    pub fn new(
        tree: &VisualContextTree,
        spaces: RasterSpacesForPlacing,
        geometries: &[RasterSpaceGeometryForPlacing],
    ) -> Self {
        let spaces_len = spaces.spaces().len();
        let spatial_node_moves_with_animation_samples = spatial_nodes_under_transform_animations(tree, |node| {
            spaces.spaces().iter().skip(1).all(|space| space.anchor != node)
        });
        let composite_form_of_clips = tree
            .clip_nodes
            .iter()
            .map(|node| {
                let ClipNodeData::Rect(data) = &node.data else {
                    return None;
                };
                if data.mode != ClipMode::Intersect || spaces.spatial_moves_on_its_own(node.spatial) {
                    return None;
                }
                let clip_space = spaces.deepest_space_of(node.spatial);
                let world = &geometries[clip_space.0].raster.world;
                translation_if_only_translates(&world.draw_matrix(node.spatial))?;
                let rect = world.map_rect_to_root(node.spatial, data.rect);
                // A clip of one axis alone clips as much at the extent limit as at its far edges on the other axis.
                let [left, top, right, bottom] = [rect.x, rect.y, rect.right(), rect.bottom()]
                    .map(|edge| edge.clamp(-EXTENT_LIMIT_IN_DEVICE_PIXELS, EXTENT_LIMIT_IN_DEVICE_PIXELS));
                let edges_are_on_whole_device_pixels = [left, top, right, bottom]
                    .iter()
                    .all(|edge| (edge - edge.round()).abs() <= COMPOSITE_CLIP_EDGE_TOLERANCE_IN_DEVICE_PIXELS);
                if !edges_are_on_whole_device_pixels {
                    return None;
                }
                Some(CompositeClip {
                    raster_space_index: clip_space,
                    rect: IntRect::new(
                        left.round() as i32,
                        top.round() as i32,
                        right.round() as i32 - left.round() as i32,
                        bottom.round() as i32 - top.round() as i32,
                    ),
                    corner_radii: data.corner_radii,
                })
            })
            .collect();
        let placement_constraint_of_effects = tree
            .effect_nodes
            .iter()
            .enumerate()
            .map(|(index, node)| {
                if tree.root_isolation_effect == Some(EffectNodeIndex(index as u32))
                    || matches!(node.data, EffectNodeData::BackgroundColorAnimation)
                {
                    return EffectPlacementConstraint::ConstrainsNothing;
                }
                let reads_backdrop = match &node.data {
                    EffectNodeData::Effects(effects) => effects.backdrop_filter.is_some(),
                    EffectNodeData::Mask(mask) => mask.masks_backdrop,
                    _ => false,
                };
                if reads_backdrop {
                    EffectPlacementConstraint::MustMoveWithSpaceAndKeepBackdropReadUncut
                } else {
                    EffectPlacementConstraint::MustMoveWithSpace
                }
            })
            .collect();
        let deepest_space_and_backdrop_filter_read_rect_by_effect = tree
            .effect_nodes
            .iter()
            .enumerate()
            .map(|(index, node)| {
                if spaces.spatial_moves_on_its_own(node.spatial) {
                    return None;
                }
                let space = spaces.deepest_space_of(node.spatial);
                match backdrop_filter_read_reach(tree, &geometries[space.0].raster, EffectNodeIndex(index as u32)) {
                    Reach::Within(read_rect) => Some((space, read_rect)),
                    _ => None,
                }
            })
            .collect();
        Self {
            spaces,
            spatial_node_moves_with_animation_samples,
            composite_form_of_clips,
            placement_constraint_of_effects,
            deepest_space_and_backdrop_filter_read_rect_by_effect,
            animated_space_holding_every_run_of_effect: Vec::new(),
            refused_animated_spaces: RefCell::new(vec![false; spaces_len]),
            effect_is_composited_around_content_of_several_spaces_by_effect_index: Vec::new(),
            cached_composite_clips_of_contexts: RefCell::new(HashMap::new()),
        }
    }

    /// Notes the effects the compositor composites around content it keeps in several spaces. Where content goes then
    /// changes, so this forgets what was worked out of it.
    pub fn note_effects_composited_around_content_of_several_spaces(&mut self, marks_by_effect_index: Vec<bool>) {
        self.effect_is_composited_around_content_of_several_spaces_by_effect_index = marks_by_effect_index;
        self.cached_composite_clips_of_contexts.borrow_mut().clear();
    }

    fn effect_is_composited_around_content_of_several_spaces(&self, effect: EffectNodeIndex) -> bool {
        self.effect_is_composited_around_content_of_several_spaces_by_effect_index
            .get(effect.0 as usize)
            .copied()
            .unwrap_or(false)
    }

    /// Notes, per effect node, the space whose anchor the compositor animates that holds every run of the list under
    /// the effect, if one does: content of that space can be placed in it under such an effect although the effect
    /// does not move with the space, as the compositor composites the effect around the space's rasters.
    pub fn note_animated_spaces_holding_every_run_of_effects(
        &mut self,
        tree: &VisualContextTree,
        list: &DisplayListStorage,
    ) {
        #[derive(Clone, Copy, PartialEq)]
        enum SpacesOfRuns {
            NoRun,
            All(RasterSpaceIndex),
            Several,
        }
        let mut spaces_of_runs_by_effect = vec![SpacesOfRuns::NoRun; tree.effect_nodes.len()];
        for run_index in 0..list.run_count() {
            let context = list.run(run_index).context;
            let space_of_run = self.deepest_space_of(context.spatial);
            let mut effect = context.effect;
            while !effect.is_none() {
                let spaces_of_runs = &mut spaces_of_runs_by_effect[effect.0 as usize];
                *spaces_of_runs = match *spaces_of_runs {
                    SpacesOfRuns::NoRun => SpacesOfRuns::All(space_of_run),
                    SpacesOfRuns::All(space) if space == space_of_run => SpacesOfRuns::All(space),
                    _ => SpacesOfRuns::Several,
                };
                effect = tree.effect_nodes[effect.0 as usize].parent;
            }
        }
        let spaces = self.spaces.spaces();
        self.animated_space_holding_every_run_of_effect = spaces_of_runs_by_effect
            .into_iter()
            .map(|spaces_of_runs| match spaces_of_runs {
                SpacesOfRuns::All(space)
                    if matches!(
                        spaces[space.0].anchor_kind,
                        RasterSpaceAnchorKind::AnimatedTransform { .. }
                    ) =>
                {
                    Some(space)
                }
                _ => None,
            })
            .collect();
    }

    pub fn animated_space_holding_every_run_of_effect(&self, effect: EffectNodeIndex) -> Option<RasterSpaceIndex> {
        self.animated_space_holding_every_run_of_effect
            .get(effect.0 as usize)
            .copied()
            .flatten()
    }

    /// Keeps content out of a space whose anchor the compositor animates, which then goes into the space around it.
    /// Called before anything is placed in the space.
    pub fn refuse_animated_space(&self, space: RasterSpaceIndex) {
        self.refused_animated_spaces.borrow_mut()[space.0] = true;
    }

    pub fn spaces(&self) -> &[RasterSpace] {
        self.spaces.spaces()
    }

    pub fn deepest_space_of(&self, spatial: SpatialNodeIndex) -> RasterSpaceIndex {
        self.spaces.deepest_space_of(spatial)
    }

    pub fn moves_with(&self, spatial: SpatialNodeIndex, space: RasterSpaceIndex) -> bool {
        self.spaces.moves_with(spatial, space)
    }

    /// The clips that rasters of `context` in `space` leave to be applied as they are composited, in the order its
    /// clip chain names them, or none where the context cannot be rastered in the space: it does not move with the
    /// space, an effect of it does not, or a clip that does not cannot be applied as a rect of whole device pixels.
    pub fn composite_clips_in(
        &self,
        tree: &VisualContextTree,
        context: ContextRef,
        space: RasterSpaceIndex,
    ) -> Option<Rc<[CompositeClip]>> {
        if let Some(cached) = self.cached_composite_clips_of_contexts.borrow().get(&(context, space)) {
            return cached.clone();
        }
        let clips = self.compute_composite_clips_in(tree, context, space).map(Rc::from);
        self.cached_composite_clips_of_contexts
            .borrow_mut()
            .insert((context, space), clips.clone());
        clips
    }

    fn compute_composite_clips_in(
        &self,
        tree: &VisualContextTree,
        context: ContextRef,
        space: RasterSpaceIndex,
    ) -> Option<Vec<CompositeClip>> {
        if !self.moves_with(context.spatial, space) || self.refused_animated_spaces.borrow()[space.0] {
            return None;
        }
        let mut composite_clips = Vec::new();
        let mut clip = context.clip;
        while !clip.is_none() {
            let node = &tree.clip_nodes[clip.0 as usize];
            let composite_form = self.composite_form_of_clips[clip.0 as usize];
            clip = node.parent;
            if self.moves_with(node.spatial, space) {
                continue;
            }
            let composite_clip = composite_form?;
            if composite_clips.contains(&composite_clip) {
                continue;
            }
            let rounded_clip_shares_space_with_another = composite_clips.iter().any(|other: &CompositeClip| {
                other.raster_space_index == composite_clip.raster_space_index
                    && (other.is_rounded() || composite_clip.is_rounded())
            });
            if rounded_clip_shares_space_with_another {
                return None;
            }
            composite_clips.push(composite_clip);
        }
        let mut effect = context.effect;
        while !effect.is_none() {
            let node = &tree.effect_nodes[effect.0 as usize];
            if self.effect_is_composited_around_content_of_several_spaces(effect) {
                effect = node.parent;
                continue;
            }
            match self.placement_constraint_of_effects[effect.0 as usize] {
                EffectPlacementConstraint::ConstrainsNothing => {}
                constraint => {
                    let composited_around_animated_space = constraint == EffectPlacementConstraint::MustMoveWithSpace
                        && self.animated_space_holding_every_run_of_effect(effect) == Some(space);
                    if !self.moves_with(node.spatial, space) && !composited_around_animated_space {
                        return None;
                    }
                    if constraint == EffectPlacementConstraint::MustMoveWithSpaceAndKeepBackdropReadUncut
                        && !self.composite_clips_cut_nothing_backdrop_filter_reads(effect, space, &composite_clips)
                    {
                        return None;
                    }
                }
            }
            effect = node.parent;
        }
        Some(composite_clips)
    }

    fn composite_clips_cut_nothing_backdrop_filter_reads(
        &self,
        effect: EffectNodeIndex,
        space: RasterSpaceIndex,
        composite_clips: &[CompositeClip],
    ) -> bool {
        if composite_clips.is_empty() {
            return true;
        }
        let Some((space_of_read_rect, read_rect)) =
            self.deepest_space_and_backdrop_filter_read_rect_by_effect[effect.0 as usize]
        else {
            return false;
        };
        if space_of_read_rect != space {
            return false;
        }
        let spaces = self.spaces.spaces();
        composite_clips.iter().all(|clip| {
            let mut read_rect_swept_over_anchor_translations = read_rect;
            let mut space_swept_through = space;
            while space_swept_through != clip.raster_space_index {
                if space_swept_through == RasterSpaceIndex::VIEWPORT {
                    return false;
                }
                read_rect_swept_over_anchor_translations = rect_swept_over_translations(
                    read_rect_swept_over_anchor_translations,
                    spaces[space_swept_through.0].lowest_anchor_translation,
                    spaces[space_swept_through.0].highest_anchor_translation,
                );
                space_swept_through = spaces[space_swept_through.0].parent_index;
            }
            clip.leaves_all_of(read_rect_swept_over_anchor_translations)
        })
    }

    /// Whether a sample of the compositor's animations can change how rasters of `space`, which `spatial` moves with,
    /// draw what is drawn in `spatial`, as a transform the compositor animates is between them.
    pub fn animation_samples_move_spatial_within_space(
        &self,
        spatial: SpatialNodeIndex,
        space: RasterSpaceIndex,
    ) -> bool {
        if self
            .spatial_node_moves_with_animation_samples
            .get(spatial.0 as usize)
            .copied()
            .unwrap_or(false)
        {
            return true;
        }
        let spaces = self.spaces.spaces();
        let mut space_of_spatial = self.deepest_space_of(spatial);
        while space_of_spatial != space && space_of_spatial != RasterSpaceIndex::VIEWPORT {
            if matches!(
                spaces[space_of_spatial.0].anchor_kind,
                RasterSpaceAnchorKind::AnimatedTransform { .. }
            ) {
                return true;
            }
            space_of_spatial = spaces[space_of_spatial.0].parent_index;
        }
        false
    }

    /// Whether what is drawn in `spatial` moves within rasters of `space`: it is in a space inside it, or moves on its
    /// own.
    pub fn spatial_moves_within_space(&self, spatial: SpatialNodeIndex, space: RasterSpaceIndex) -> bool {
        let deepest = self.deepest_space_of(spatial);
        (deepest != space && space_is_ancestor_or_self(self.spaces.spaces(), space, deepest))
            || self.spaces.spatial_moves_on_its_own(spatial)
    }

    /// Whether content of `context` rastered in `space` moves within it as an anchor of a space inside it scrolls, a
    /// sticky box sticks or the compositor animates a transform or a filter, so that where it lands holds at the
    /// current values only.
    pub fn context_moves_within_space(
        &self,
        tree: &VisualContextTree,
        context: ContextRef,
        space: RasterSpaceIndex,
    ) -> bool {
        let moves_within_space = |spatial: SpatialNodeIndex| self.spatial_moves_within_space(spatial, space);
        if self.spaces.effect_spread_moves_on_its_own(context.effect) {
            return true;
        }
        if moves_within_space(context.spatial) {
            return true;
        }
        let mut clip = context.clip;
        while !clip.is_none() {
            let node = &tree.clip_nodes[clip.0 as usize];
            if moves_within_space(node.spatial) {
                return true;
            }
            clip = node.parent;
        }
        let mut effect = context.effect;
        while !effect.is_none() {
            let node = &tree.effect_nodes[effect.0 as usize];
            if tree.root_isolation_effect != Some(effect)
                && !self.effect_is_composited_around_content_of_several_spaces(effect)
                && moves_within_space(node.spatial)
            {
                return true;
            }
            effect = node.parent;
        }
        false
    }
}

/// The records of runs that a space whose anchor what they draw does not move with cannot raster: records with inline
/// clips in spaces of their own, and scroll bars, alone in their runs, whose thumbs follow a scroll offset, as the
/// chunks they are in know.
pub(super) struct RunsReachingOutsideTheirContext {
    inline_clip_spaces_of_runs: HashMap<usize, Vec<SpatialNodeIndex>>,
    scroll_nodes_moving_scroll_bar_thumbs_of_runs: HashMap<usize, Vec<SpatialNodeIndex>>,
}

impl RunsReachingOutsideTheirContext {
    pub fn new(list: &DisplayListStorage) -> Self {
        let mut inline_clip_spaces_of_runs: HashMap<usize, Vec<SpatialNodeIndex>> = HashMap::new();
        let mut scroll_nodes_moving_scroll_bar_thumbs_of_runs: HashMap<usize, Vec<SpatialNodeIndex>> = HashMap::new();
        for (chunk_index, chunk) in list.chunks().iter().enumerate() {
            let first_run_index_of_chunk = list.first_run_of_chunk(ChunkIndexInList(chunk_index as u32));
            for record in chunk.records_reaching_outside_run_context() {
                let run_index = first_run_index_of_chunk + record.run_index as usize;
                match record.reach_outside_run_context {
                    ReachOutsideRunContext::InlineClipInSpaceOf(space) => {
                        inline_clip_spaces_of_runs.entry(run_index).or_default().push(space);
                    }
                    ReachOutsideRunContext::ScrollbarThumbFollowsScrollOffsetOf(scroll_node_index) => {
                        scroll_nodes_moving_scroll_bar_thumbs_of_runs
                            .entry(run_index)
                            .or_default()
                            .push(scroll_node_index);
                    }
                }
            }
        }
        Self {
            inline_clip_spaces_of_runs,
            scroll_nodes_moving_scroll_bar_thumbs_of_runs,
        }
    }

    /// The clips rasters of the run in `space` leave to be composited, or none where the run cannot be rastered there.
    pub fn composite_clips_of_run_in(
        &self,
        tree: &VisualContextTree,
        tables: &PlacementTables,
        run_index: usize,
        run: &DisplayListCommandRun,
        space: RasterSpaceIndex,
    ) -> Option<Rc<[CompositeClip]>> {
        let composite_clips = tables.composite_clips_in(tree, run.context, space)?;
        let has_inline_clip_not_moving_with_space =
            self.inline_clip_spaces_of_runs
                .get(&run_index)
                .is_some_and(|clip_spaces| {
                    clip_spaces.iter().any(|&clip_space| {
                        (clip_space.0 as usize) < tree.spatial_nodes.len() && !tables.moves_with(clip_space, space)
                    })
                });
        if has_inline_clip_not_moving_with_space {
            return None;
        }
        let has_scroll_bar_thumb_following_anchor_of_space = self
            .scroll_nodes_moving_scroll_bar_thumbs_of_runs
            .get(&run_index)
            .is_some_and(|scroll_nodes| {
                space_and_ancestors_below_viewport(tables.spaces(), space)
                    .any(|space_or_ancestor| scroll_nodes.contains(&tables.spaces()[space_or_ancestor.0].anchor))
            });
        if has_scroll_bar_thumb_following_anchor_of_space {
            return None;
        }
        Some(composite_clips)
    }

    /// Whether an inline clip of the run in a space of its own moves within rasters of `space`, which then read where
    /// the clip is.
    pub fn inline_clips_of_run_move_within_space(
        &self,
        tree: &VisualContextTree,
        tables: &PlacementTables,
        run_index: usize,
        space: RasterSpaceIndex,
    ) -> bool {
        self.inline_clip_spaces_of_runs
            .get(&run_index)
            .is_some_and(|clip_spaces| {
                clip_spaces.iter().any(|&clip_space| {
                    (clip_space.0 as usize) < tree.spatial_nodes.len()
                        && tables.spatial_moves_within_space(clip_space, space)
                })
            })
    }

    /// The deepest space the run can be rastered in.
    pub fn deepest_space_of_run(
        &self,
        tree: &VisualContextTree,
        tables: &PlacementTables,
        run_index: usize,
        run: &DisplayListCommandRun,
    ) -> RasterSpaceIndex {
        let mut space = tables.deepest_space_of(run.context.spatial);
        while space != RasterSpaceIndex::VIEWPORT
            && self
                .composite_clips_of_run_in(tree, tables, run_index, run, space)
                .is_none()
        {
            space = tables.spaces()[space.0].parent_index;
        }
        space
    }
}
