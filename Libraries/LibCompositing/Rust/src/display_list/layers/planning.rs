/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Plans the layers of a display list. WebContent's Paint thread plans them and sends the plan to the compositor. The
//! compositor takes a plan as it is, once the plan passes the checks of its wire form, and never plans.
//!
//! A band of a nested node ends where content outside it that can show where the band can is painted, so the layers
//! stacked in paint order of their barriers paint every pixel in the order the list does, as long as nothing within a
//! layer reads what is below the layer. So a backdrop filter that finds all it reads in its own layer, where it is
//! painted, reads it there, and one that does not gets a node of its own, whose layers the compositor composites inside
//! the filter's effect as a replay of the list composites the effect, over what is composited below them. So does an
//! effect whose opacity or filter the compositor animates, so that its samples change how its layers are composited and
//! not their tiles.

use super::wire::{
    CANVAS_VIDEO_AND_NESTED_CONTEXT_LAYERS_A_BAND_LEAVES_CUTS_FOR, plan_holds_more_than_compositor_decodes,
};
use super::{
    BandOfPlan, BarrierCut, BarrierOfPlan, CutOfPlan, LayerIndex, LayerStack, LayeredPlan, LayeringObstacles,
    MAXIMUM_LAYER_COUNT_OF_LIVE_CONTENT_DRAWN_BY_ITS_RECORD, MAXIMUM_NESTING_OF_COMPOSITED_EFFECTS, NodeOfPlan,
    PlacementNodeIndex, StackOnWire, WhyCompositorCannotCompositeEffect, WhyListIsOneLayer, media_name,
};
use crate::display_list::animated_transform_spaces::{effect_reads_below_it, spatial_nodes_under_transform_animations};
use crate::display_list::builder::{bounding_rect_computed_from_payload, record_can_paint_within};
use crate::display_list::commands::{EffectNodeIndex, SpatialNodeIndex};
use crate::display_list::effect_geometry::{
    FiltersOfEffects, KeyframeFiltersOfEffects, backdrop_filter_read_reach, filter_may_spread_at_some_sample,
    filter_read_reach_in_device_pixels,
};
use crate::display_list::placement::{
    ANTIALIASING_MARGIN_IN_DEVICE_PIXELS, CompositeClip, FiltersAndClipsOfList, RasterSpace, RasterSpaceAnchorKind,
    RasterSpaceIndex, RunGroupIndex, RunGroups, compositor_scroll_record_of, enclosing_int_rect_within_extent_limit,
    is_finite_rect,
};
use crate::display_list::reach::Reach;
use crate::display_list::storage::{DisplayListStorage, ListPosition, ListSpan, RunIndexInChunk, RunIndexInList};
use crate::display_list::summary::{IndexedRecord, LiveContent};
use crate::display_list::tiles::{CellRange, reaches_too_many_cells_to_follow_one_by_one, tile_coords_touching};
use crate::visual_context::queries::{BackdropReader, NestingOfLiveEffects, backdrop_reader_by_reading_effect_index};
use crate::visual_context::{EffectNodeData, VisualContextTree};
use libgfx_rust::{FloatPoint, FloatRect, IntRect};
use std::collections::HashMap;
use std::hash::Hash;
use std::sync::Arc;

mod lines_of_plan;
mod placement_tables;
mod raster_spaces_for_placing;

use placement_tables::{PlacementTables, RunsReachingOutsideTheirContext, lowest_common_ancestor_space};
use raster_spaces_for_placing::{
    RasterSpaceAnchorsOfTree, RasterSpaceGeometryForPlacing, RasterSpacesForPlacing, rect_swept_over_translations,
    spaces_with_geometries,
};

/// How many effects whose opacity or filter the compositor animates get layers of their own, first in paint order.
pub const MAXIMUM_ANIMATED_NODES_WITH_LAYERS: usize = 32;

/// A node whose values the compositor animates: a spatial node it transforms, or an effect node whose opacity or
/// filter it changes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AnimatedNode {
    Transform(SpatialNodeIndex),
    Effect(EffectNodeIndex),
}

/// Why what the compositor animates stays in the tiles of the layers around it, which it keys anew as it changes,
/// rather than getting layers of its own.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WhyAnimationHasNoLayer {
    InsideAnEffectTheCompositorCannotComposite(WhyCompositorCannotCompositeEffect),
    ItsFilterSpreadsWhatItFilters,
    ItHoldsAnEffectThatReadsWhatIsBelowIt,
    ItHoldsRecordsThatBlendWithWhatIsBelowThem,
    ALaterEffectReadsItAsBackdrop,
    SortedByDepthInA3DSortingContext,
    NothingBoundsWhereItMoves,
    ItCanShowOverAnEarlierCanvasOrVideo,
    MoreAnimationsThanLayersFor,
}

impl std::fmt::Display for WhyAnimationHasNoLayer {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(match self {
            WhyAnimationHasNoLayer::InsideAnEffectTheCompositorCannotComposite(why) => {
                return write!(formatter, "inside a {why}");
            }
            WhyAnimationHasNoLayer::ItsFilterSpreadsWhatItFilters => "its filter spreads what it filters",
            WhyAnimationHasNoLayer::ItHoldsAnEffectThatReadsWhatIsBelowIt => {
                "it holds an effect that reads what is below it"
            }
            WhyAnimationHasNoLayer::ItHoldsRecordsThatBlendWithWhatIsBelowThem => {
                "it holds records that blend with what is below them"
            }
            WhyAnimationHasNoLayer::ALaterEffectReadsItAsBackdrop => "a later effect reads it as backdrop",
            WhyAnimationHasNoLayer::SortedByDepthInA3DSortingContext => "sorted by depth in a 3D sorting context",
            WhyAnimationHasNoLayer::NothingBoundsWhereItMoves => "nothing bounds where it moves",
            WhyAnimationHasNoLayer::ItCanShowOverAnEarlierCanvasOrVideo => {
                "it can show over an earlier canvas or video"
            }
            WhyAnimationHasNoLayer::MoreAnimationsThanLayersFor => "past the most animations that get layers",
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WhyMediaHasNoLayer {
    DrawnInsideAnotherCommand,
    UnderTransformThatIsNot2DAffine,
    DrawsNothingVisible,
    MoreMediaThanLayersFor,
    SortedByDepthInA3DSortingContext,
    InsideAnEffectTheCompositorCannotComposite(WhyCompositorCannotCompositeEffect),
    InsideAnEffectThatPushesALayerAtReplay,
    MovesWithinItsSpace,
}

impl std::fmt::Display for WhyMediaHasNoLayer {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(match self {
            WhyMediaHasNoLayer::DrawnInsideAnotherCommand => "drawn inside another command",
            WhyMediaHasNoLayer::UnderTransformThatIsNot2DAffine => "under a transform that is not 2D affine",
            WhyMediaHasNoLayer::DrawsNothingVisible => "draws nothing visible",
            WhyMediaHasNoLayer::MoreMediaThanLayersFor => "past the most media that get layers",
            WhyMediaHasNoLayer::SortedByDepthInA3DSortingContext => "sorted by depth in a 3D sorting context",
            WhyMediaHasNoLayer::InsideAnEffectTheCompositorCannotComposite(why) => {
                return write!(formatter, "inside a {why}");
            }
            WhyMediaHasNoLayer::InsideAnEffectThatPushesALayerAtReplay => {
                "inside an effect that pushes a layer at replay"
            }
            WhyMediaHasNoLayer::MovesWithinItsSpace => "moves within its space",
        })
    }
}

/// Why the compositor composites the layers of an effect's content inside the effect, as a replay of the list
/// composites the effect, rather than tiles holding the effect.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WhyEffectIsComposited {
    ItReadsWhatItsOwnLayerDoesNotHold,
    ItsBackdropFilterReadsPastTheEdgeOfTheFrame,
    ItHoldsAnEffectTheCompositorComposites,
    ItHoldsMedia(LiveContent),
    TheCompositorAnimatesIt,
    ItAppliesOverATransformTheCompositorAnimates,
    ItHoldsContentOfSeveralSpaces,
}

impl std::fmt::Display for WhyEffectIsComposited {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(match self {
            WhyEffectIsComposited::ItReadsWhatItsOwnLayerDoesNotHold => "it reads what its own layer does not hold",
            WhyEffectIsComposited::ItsBackdropFilterReadsPastTheEdgeOfTheFrame => {
                "its backdrop filter reads past the edge of the frame"
            }
            WhyEffectIsComposited::ItHoldsAnEffectTheCompositorComposites => {
                "it holds an effect the compositor composites"
            }
            WhyEffectIsComposited::ItHoldsMedia(content) => {
                return write!(formatter, "it holds a {} with a layer of its own", media_name(*content));
            }
            WhyEffectIsComposited::TheCompositorAnimatesIt => "the compositor animates it",
            WhyEffectIsComposited::ItAppliesOverATransformTheCompositorAnimates => {
                "it applies over a transform the compositor animates"
            }
            WhyEffectIsComposited::ItHoldsContentOfSeveralSpaces => "it holds content of several spaces",
        })
    }
}

// A backdrop reader that a composited effect painted before it, within its group, would get composited.
struct BackdropReaderAnAnimationMayCut {
    reader: EffectNodeIndex,
    backdrop_root: EffectNodeIndex,
    first_run_index: usize,
    read_tiles: Reach<IntRect>,
}

/// A plan of the layers of a list: the stack the compositor composites frames from, what of the stack a plan carries
/// to the compositor, and where the parts of the list are placed among the spaces of the stack, which only the dump,
/// the overlay of layers and the tests read.
#[derive(Clone, Debug, PartialEq)]
pub struct PlannedLayers {
    pub stack: LayerStack,
    pub stack_on_wire: StackOnWire,
    pub placement: ListPlacement,
}

/// What a plan of the layers of a list reads, all of which WebContent publishes.
pub struct LayerStackInputs<'a> {
    pub list: &'a DisplayListStorage,
    /// The tree as WebContent published it, without the samples of the compositor's animations, with its visual
    /// viewport at identity: plans are made in the device pixels of the layout viewport and hold wherever the visual
    /// viewport is.
    pub tree: &'a VisualContextTree,
    /// The scroll offsets WebContent published with the tree. A plan reads only those that
    /// `RasterSpaceAnchorsOfTree::leaves_out_scroll_offset_of_node` does not name.
    pub scroll_offsets: &'a [FloatPoint],
    /// The viewport in device pixels.
    pub viewport_rect: IntRect,
}

// An animation of the opacity or filter of the root isolation effect makes it more than a plain group at some sample.
fn root_isolation_effect_is_plain_group(tree: &VisualContextTree) -> bool {
    let Some(root) = tree.root_isolation_effect else {
        return true;
    };
    match &tree.effect_nodes[root.0 as usize].data {
        EffectNodeData::Effects(effects) => {
            effects.opacity == 1.0
                && effects.blend_mode == libgfx_rust::CompositingAndBlendingOperator::Normal
                && effects.filter.is_none()
                && effects.backdrop_filter.is_none()
                && !tree.effect_opacity_or_filter_is_animated(root)
        }
        _ => false,
    }
}

/// Why a list cannot be layered at all.
fn why_list_cannot_be_layered(inputs: &LayerStackInputs<'_>) -> Option<WhyListIsOneLayer> {
    if inputs.viewport_rect.is_empty() {
        return Some(WhyListIsOneLayer::EmptyViewport);
    }
    if inputs.list.run_count() == 0 {
        return Some(WhyListIsOneLayer::EmptyDisplayList);
    }
    if !root_isolation_effect_is_plain_group(inputs.tree) {
        return Some(WhyListIsOneLayer::RootIsolationEffectIsNotPlainGroup);
    }
    None
}

/// A group of runs, which is placed whole, with where it is placed.
#[derive(Clone, Debug, PartialEq)]
pub struct PlacedRunGroup {
    pub raster_space_index: RasterSpaceIndex,
    /// At most one per space, ordered by space.
    pub composite_clips: Vec<CompositeClip>,
    /// Where it can show in the space's raster coordinates, under the values the frame shows, except that content
    /// that moves on its own is taken to be anywhere its fixed clips let it show.
    pub reach: IntRect,
    pub moves_within_space: bool,
    /// Whether the group paints within the root group, rather than below it as the canvas background is.
    pub paints_within_root_group: bool,
    /// In the space's raster coordinates.
    pub reach_read_by_records_blending_with_root_group: Reach<IntRect>,
    /// The innermost effect around the group that holds content of several spaces, inside whose node the node of the
    /// group goes.
    pub inside_effect_holding_content_of_several_spaces: Option<EffectNodeIndex>,
}

/// The index of a placed effect in `ListPlacement::placed_effects`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct PlacedEffectIndex(pub usize);

/// The index of a content in `ListPlacement::placed_media`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct PlacedMediaIndex(pub usize);

/// A stretch of records of one run group that is placed as one: the records of the run group's runs that are inside
/// the same effects the compositor can composite, or the runs of a content that is a layer of its own, which hold it
/// alone and may span run groups.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PlacedRecordStretch {
    pub record_span: ListSpan,
    /// The run group of its first run.
    pub run_group_index: RunGroupIndex,
    /// Where the records can show, in the raster coordinates of the run group's space: where the innermost placed
    /// effect around them can draw, if there is one.
    pub reach: IntRect,
    pub placed_media_index: Option<PlacedMediaIndex>,
    /// The innermost effect the compositor can composite the stretch's records inside.
    pub placed_effect_index: Option<PlacedEffectIndex>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
struct RecordStretchIndex(usize);

/// An effect whose content the compositor composites inside it, or may where the content does not find all it reads
/// in the layer it goes into, with where it is placed.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PlacedEffect {
    pub effect: EffectNodeIndex,
    /// The placed effect around it, inside which the compositor composites it.
    pub placed_effect_around_index: Option<PlacedEffectIndex>,
    /// Where compositing the effect can draw, in the raster coordinates of the run group's space: wherever the runs
    /// under it in the run group of its first run can show, and if it moves within its space, wherever it can show.
    pub reach: IntRect,
    /// What it reads of what is composited below it there.
    pub backdrop_read_reach: Reach<IntRect>,
    /// None for an effect placed only as it reads the content of the root group, which the bands it can read are only
    /// known of as the stack is cut into them.
    pub why_composited_regardless_of_layer_content: Option<WhyEffectIsComposited>,
    /// For an effect that holds content of several spaces, the space of its node: the lowest space holding all of that
    /// content, whose raster coordinates `reach` is in. Each run group of its content goes into a node of its own
    /// space inside that node, rather than all of them into one space where most of them would move.
    pub space_of_node_holding_content_of_several_spaces: Option<RasterSpaceIndex>,
}

/// Live content that is a layer of its own, alone in its run.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PlacedMedia {
    pub content: LiveContent,
    /// How many drawings of the same content are painted before it.
    pub ordinal: u32,
    pub record_span: ListSpan,
    pub run_index: RunIndexInList,
    /// Where it can show, in the raster coordinates of the space of its run's group.
    pub reach: IntRect,
    /// Whether a record painted after it can show where it can, so that it cuts the tiles it touches. Where none can, it
    /// cuts none, and the records of its band painted after it stay in the layers below it, such as the base layer,
    /// where text takes LCD antialiasing.
    pub cuts_band: bool,
}

/// Where each part of a list is placed at the spaces a tree offers, under the values a frame shows. What it decides
/// holds for every sample of the animations of the tree.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ListPlacement {
    pub run_groups: Vec<PlacedRunGroup>,
    pub record_stretches: Vec<PlacedRecordStretch>,
    /// In paint order.
    pub placed_media: Vec<PlacedMedia>,
    pub media_without_layers: Vec<(LiveContent, WhyMediaHasNoLayer)>,
    pub animations_without_layers: Vec<(AnimatedNode, WhyAnimationHasNoLayer)>,
    /// In paint order, each after the one around it.
    pub placed_effects: Vec<PlacedEffect>,
    /// The nodes that read the backdrop of a backdrop filter or a blend mode, in paint order.
    pub backdrop_readers_in_paint_order: Vec<EffectNodeIndex>,
    pub why_compositor_cannot_composite_by_effect_index: Vec<Option<WhyCompositorCannotCompositeEffect>>,
}

fn one_composite_clip_per_space_sorted_by_space(clips: &[CompositeClip]) -> Vec<CompositeClip> {
    let mut clip_per_space: Vec<CompositeClip> = Vec::with_capacity(clips.len());
    for clip in clips {
        match clip_per_space
            .iter_mut()
            .find(|clip_of_same_space| clip_of_same_space.raster_space_index == clip.raster_space_index)
        {
            Some(clip_of_same_space) => clip_of_same_space.rect = clip_of_same_space.rect.intersected(clip.rect),
            None => clip_per_space.push(*clip),
        }
    }
    clip_per_space.sort_by_key(|clip| clip.raster_space_index);
    clip_per_space
}

// Marks in each of `marks_by_effect_index` every effect node that holds a node marked there, in one walk of the tree.
fn mark_effects_around_marked_effects<const MARK_KIND_COUNT: usize>(
    tree: &VisualContextTree,
    marks_by_effect_index: &mut [Vec<bool>; MARK_KIND_COUNT],
) {
    let mut live_effects_parents_first = Vec::with_capacity(tree.effect_nodes.len());
    tree.visit_effect_nodes_parents_first(|index| live_effects_parents_first.push(index));
    for &index in live_effects_parents_first.iter().rev() {
        let parent = tree.effect_nodes[index].parent;
        if parent.is_none() {
            continue;
        }
        for marked_by_effect_index in marks_by_effect_index.iter_mut() {
            if marked_by_effect_index[index] {
                marked_by_effect_index[parent.0 as usize] = true;
            }
        }
    }
}

// Marks the effect of each run that `marks_run` marks.
fn effects_of_marked_runs(
    list: &DisplayListStorage,
    tree: &VisualContextTree,
    marks_run: impl Fn(usize) -> bool,
) -> Vec<bool> {
    let mut marked_by_effect_index = vec![false; tree.effect_nodes.len()];
    for (run_index, run) in list.runs().enumerate() {
        if !run.context.effect.is_none() && marks_run(run_index) {
            marked_by_effect_index[run.context.effect.0 as usize] = true;
        }
    }
    marked_by_effect_index
}

// Per effect node, whether it is or holds the effect of a run sorted by depth, of a record that blends with what is
// below it, or an effect that reads what is below it.
struct WhatEffectsHold {
    run_sorted_by_depth_by_effect_index: Vec<bool>,
    records_blending_with_what_is_below_them_by_effect_index: Vec<bool>,
    effect_reading_what_is_below_it_by_effect_index: Vec<bool>,
}

// Why each effect is composited whatever its layers hold, and the effects that are, in the order they became so.
struct WhyEffectsAreCompositedRegardlessOfLayerContent {
    why_by_effect_index: Vec<Option<WhyEffectIsComposited>>,
    composited_effects: Vec<EffectNodeIndex>,
}

impl WhyEffectsAreCompositedRegardlessOfLayerContent {
    fn set(&mut self, effect: EffectNodeIndex, why: WhyEffectIsComposited) {
        let why_composited = &mut self.why_by_effect_index[effect.0 as usize];
        if why_composited.is_none() {
            self.composited_effects.push(effect);
        }
        *why_composited = Some(why);
    }

    fn set_unless_composited(&mut self, effect: EffectNodeIndex, why: WhyEffectIsComposited) {
        if self.why_by_effect_index[effect.0 as usize].is_none() {
            self.set(effect, why);
        }
    }

    fn mark_effects_around_as_composited(
        &mut self,
        tree: &VisualContextTree,
        mut effect_around: EffectNodeIndex,
        why: WhyEffectIsComposited,
    ) {
        while !effect_around.is_none() && tree.root_isolation_effect != Some(effect_around) {
            if tree.effect_pushes_layer_at_replay(effect_around) {
                self.set_unless_composited(effect_around, why);
            }
            effect_around = tree.effect_nodes[effect_around.0 as usize].parent;
        }
    }
}

/// Whether a backdrop filter reads nothing past each pixel, or only through blurs that mirror at the edge of what they
/// read, so that what it reads past the edge of the frame does not show.
fn backdrop_filter_reads_nothing_past_mirrored_edge(filter: &[u8]) -> bool {
    let probe = FloatRect::new(0.0, 0.0, 1.0, 1.0);
    let reads_past_each_pixel = crate::filter_bytes::changed_output_bounds(filter, probe) != Some(probe);
    !reads_past_each_pixel || libgfx_rust::filter::Filter::serialized_has_mirroring_blur(filter)
}

/// Where each part of the list is placed among `spaces`, given the geometry of each of them, with what the stack holds
/// per space of them: `LayerStack::swept_reach_in_parent_space_by_space_index` and
/// `LayerStack::tiles_hold_content_moving_within_space_by_space_index`. Canvases, videos and nested contexts past
/// `most_canvas_video_and_nested_context_layers`, in paint order, get no layers.
fn place_list(
    inputs: &LayerStackInputs<'_>,
    spaces: RasterSpacesForPlacing,
    geometries: &[RasterSpaceGeometryForPlacing],
    nesting_of_live_effects: &NestingOfLiveEffects<'_>,
    most_canvas_video_and_nested_context_layers: Option<usize>,
) -> (ListPlacement, Vec<Option<IntRect>>, Vec<bool>) {
    let tree = inputs.tree;
    // Placing decides for the filter that the tree holds and for each filter that an animation can give the effect.
    let filters_at_some_sample = FiltersOfEffects::of(tree, None, true);
    let filters_and_clips = FiltersAndClipsOfList {
        tree,
        filters: &filters_at_some_sample,
        effect_clips: inputs.list.effect_clip_plan(tree),
    };
    let placing = ListBeingPlaced::new(inputs, spaces, geometries, nesting_of_live_effects, filters_and_clips);
    let (runs, mut animations_without_layers) = placing.place_run_groups();
    let (placed_media, media_without_layers) = placing.place_media(&runs, most_canvas_video_and_nested_context_layers);
    let mut why_composited_regardless_of_layer_content =
        placing.effects_composited_regardless_of_layer_content(&runs, &placed_media, &mut animations_without_layers);
    let (backdrop_read_reach_by_effect_index, backdrop_readers_in_paint_order, reads_root_group_by_effect_index) =
        placing.note_backdrop_readers(&runs, &placed_media, &mut why_composited_regardless_of_layer_content);
    let (placed_effects, innermost_placed_effect_at_or_around_by_effect_index) = placing.place_effects(
        &runs,
        &why_composited_regardless_of_layer_content,
        &backdrop_read_reach_by_effect_index,
        &reads_root_group_by_effect_index,
    );
    let (record_stretches, tiles_hold_content_moving_within_space_by_space_index) = placing.place_record_stretches(
        &runs,
        &placed_media,
        &placed_effects,
        &innermost_placed_effect_at_or_around_by_effect_index,
    );
    let placement = ListPlacement {
        run_groups: runs.groups,
        record_stretches,
        placed_media,
        media_without_layers,
        animations_without_layers,
        placed_effects,
        backdrop_readers_in_paint_order,
        why_compositor_cannot_composite_by_effect_index: placing.why_compositor_cannot_composite_by_effect_index,
    };
    (
        placement,
        runs.swept_reach_in_parent_space_by_space_index,
        tiles_hold_content_moving_within_space_by_space_index,
    )
}

fn is_animated_space(space: &RasterSpace) -> bool {
    matches!(space.anchor_kind, RasterSpaceAnchorKind::AnimatedTransform { .. })
}

// What placing a list reads: its inputs, and what is worked out of them alone before anything is placed.
struct ListBeingPlaced<'a> {
    tree: &'a VisualContextTree,
    list: &'a DisplayListStorage,
    run_groups: Arc<RunGroups>,
    tables: PlacementTables,
    geometries: &'a [RasterSpaceGeometryForPlacing],
    nesting_of_live_effects: &'a NestingOfLiveEffects<'a>,
    filters_and_clips: FiltersAndClipsOfList<'a>,
    some_space_is_animated: bool,
    why_compositor_cannot_composite_by_effect_index: Vec<Option<WhyCompositorCannotCompositeEffect>>,
    why_compositor_cannot_composite_effect_or_effect_around_by_effect_index:
        std::cell::OnceCell<Vec<Option<WhyCompositorCannotCompositeEffect>>>,
    backdrop_reader_by_effect_index: Vec<Option<BackdropReader>>,
    first_run_index_by_effect_index: Vec<usize>,
    effects_in_paint_order_with_outer_before_inner: Vec<EffectNodeIndex>,
    runs_reaching_outside: RunsReachingOutsideTheirContext,
    /// Empty where no effect holds content of several spaces.
    effect_holds_content_of_several_spaces_by_effect_index: Vec<bool>,
}

// Where the node of an effect that holds content of several spaces is, as `PlacedEffect` tells it.
#[derive(Clone, Copy)]
struct NodeOfEffectHoldingContentOfSeveralSpaces {
    space: RasterSpaceIndex,
    reach: IntRect,
}

// What placing the run groups of a list works out, which the phases after it read.
struct RunGroupsPlaced {
    groups: Vec<PlacedRunGroup>,
    reach_of_run_by_run_index: Vec<IntRect>,
    run_moves_within_space_by_run_index: Vec<bool>,
    swept_reach_in_parent_space_by_space_index: Vec<Option<IntRect>>,
    reach_by_effect_index_once_worked_out: std::cell::OnceCell<Vec<IntRect>>,
    /// Empty where no effect holds content of several spaces.
    node_by_effect_holding_content_of_several_spaces: Vec<Option<NodeOfEffectHoldingContentOfSeveralSpaces>>,
}

impl<'a> ListBeingPlaced<'a> {
    fn new(
        inputs: &LayerStackInputs<'a>,
        spaces: RasterSpacesForPlacing,
        geometries: &'a [RasterSpaceGeometryForPlacing],
        nesting_of_live_effects: &'a NestingOfLiveEffects<'a>,
        filters_and_clips: FiltersAndClipsOfList<'a>,
    ) -> Self {
        let (tree, list) = (inputs.tree, inputs.list);
        let mut tables = PlacementTables::new(tree, spaces, geometries);
        let some_space_is_animated = tables.spaces().iter().any(is_animated_space);
        if some_space_is_animated {
            tables.note_animated_spaces_holding_every_run_of_effects(tree, list);
        }
        let keyframe_filters = KeyframeFiltersOfEffects::of(tree);
        // A space whose anchor the compositor animates rasters at a scale of its own, so how far a filter reads in the
        // pixels of tiles is told by the deepest space of the filter. Where a transform the compositor animates within
        // that space scales the filter, it is not known.
        let why_compositor_cannot_composite_by_effect_index: Vec<Option<WhyCompositorCannotCompositeEffect>> = tree
            .effect_nodes
            .iter()
            .enumerate()
            .map(|(index, effect_node)| {
                let effect = EffectNodeIndex(index as u32);
                if keyframe_filters.filter_may_move_what_it_filters(tree, effect) {
                    return Some(WhyCompositorCannotCompositeEffect::FilterMovesWhatItFilters);
                }
                if !filter_may_spread_at_some_sample(tree, effect) {
                    return None;
                }
                let deepest_space = tables.deepest_space_of(effect_node.spatial);
                let read_reach_in_device_pixels = (!tables
                    .animation_samples_move_spatial_within_space(effect_node.spatial, deepest_space))
                .then(|| {
                    keyframe_filters.filter_read_reach_in_device_pixels(
                        tree,
                        effect,
                        &geometries[deepest_space.0]
                            .raster
                            .world
                            .draw_matrix(effect_node.spatial),
                    )
                })
                .flatten();
                read_reach_in_device_pixels
                    .is_none()
                    .then_some(WhyCompositorCannotCompositeEffect::FilterReadsFartherThanTilesAreFollowed)
            })
            .collect();
        let mut first_run_index_by_effect_index = vec![usize::MAX; tree.effect_nodes.len()];
        let mut effects_in_paint_order_with_outer_before_inner = Vec::new();
        for run_index in 0..list.run_count() {
            let mut effect = list.run(run_index).context.effect;
            let mut newly_reached_effects_inner_to_outer = Vec::new();
            while !effect.is_none() && first_run_index_by_effect_index[effect.0 as usize] == usize::MAX {
                first_run_index_by_effect_index[effect.0 as usize] = run_index;
                newly_reached_effects_inner_to_outer.push(effect);
                effect = tree.effect_nodes[effect.0 as usize].parent;
            }
            effects_in_paint_order_with_outer_before_inner
                .extend(newly_reached_effects_inner_to_outer.into_iter().rev());
        }
        let mut placing = Self {
            tree,
            list,
            run_groups: list.run_groups(tree),
            tables,
            geometries,
            nesting_of_live_effects,
            filters_and_clips,
            some_space_is_animated,
            why_compositor_cannot_composite_by_effect_index,
            why_compositor_cannot_composite_effect_or_effect_around_by_effect_index: std::cell::OnceCell::new(),
            backdrop_reader_by_effect_index: backdrop_reader_by_reading_effect_index(tree),
            first_run_index_by_effect_index,
            effects_in_paint_order_with_outer_before_inner,
            runs_reaching_outside: RunsReachingOutsideTheirContext::new(list),
            effect_holds_content_of_several_spaces_by_effect_index: Vec::new(),
        };
        placing.note_effects_holding_content_of_several_spaces();
        placing
    }

    fn effect_holds_content_of_several_spaces(&self, effect: EffectNodeIndex) -> bool {
        self.effect_holds_content_of_several_spaces_by_effect_index
            .get(effect.0 as usize)
            .copied()
            .unwrap_or(false)
    }

    // A group effect placed whole is rastered in the lowest space holding all of its content, where content of the
    // spaces inside that one moves as they scroll or stick, and is rastered anew for every frame they move in. So an
    // effect whose content is in several spaces is composited around nodes of those spaces instead, where the
    // compositor can composite it, which a group effect around it must be too. That leaves out an effect that reads
    // what is below it or that content inside it reads, as what each reads is told in the space of its run group.
    fn note_effects_holding_content_of_several_spaces(&mut self) {
        let (tree, list) = (self.tree, self.list);
        let holds_runs_of_kind = |marks_run: &dyn Fn(usize) -> bool| {
            let mut marks = [effects_of_marked_runs(list, tree, marks_run)];
            mark_effects_around_marked_effects(tree, &mut marks);
            let [marked_by_effect_index] = marks;
            marked_by_effect_index
        };
        let mut runs_holding_records_blending_with_what_is_below_them = vec![false; list.run_count()];
        for record in &list.summary().destination_blending_records {
            runs_holding_records_blending_with_what_is_below_them[record.run_index as usize] = true;
        }
        let holds_records_blending_with_what_is_below_them =
            holds_runs_of_kind(&|run_index| runs_holding_records_blending_with_what_is_below_them[run_index]);
        let holds_runs_sorted_by_depth =
            holds_runs_of_kind(&|run_index| self.run_groups.run_is_sorted_by_depth(run_index));
        let groups_runs = |effect: EffectNodeIndex| {
            tree.effect_nodes[effect.0 as usize].data.pushes_layer() && tree.root_isolation_effect != Some(effect)
        };
        let mut may_hold_content_of_several_spaces = vec![false; tree.effect_nodes.len()];
        tree.visit_effect_nodes_parents_first(|index| {
            let effect = EffectNodeIndex(index as u32);
            let node = &tree.effect_nodes[index];
            let EffectNodeData::Effects(effects) = &node.data else {
                return;
            };
            let every_group_effect_around_may = node.parent.is_none()
                || !groups_runs(node.parent)
                || may_hold_content_of_several_spaces[node.parent.0 as usize];
            may_hold_content_of_several_spaces[index] = every_group_effect_around_may
                && groups_runs(effect)
                && effects.blend_mode == libgfx_rust::CompositingAndBlendingOperator::Normal
                && effects.backdrop_filter.is_none()
                && self.backdrop_reader_by_effect_index[index].is_none()
                && !filter_may_spread_at_some_sample(tree, effect)
                && self
                    .why_compositor_cannot_composite_effect_or_effects_around(effect)
                    .is_none()
                && !holds_records_blending_with_what_is_below_them[index]
                && !holds_runs_sorted_by_depth[index];
        });
        if !may_hold_content_of_several_spaces.contains(&true) {
            return;
        }

        // Where an effect is composited around its content, the content can go into spaces deeper than the effect's.
        self.tables
            .note_effects_composited_around_content_of_several_spaces(may_hold_content_of_several_spaces.clone());
        #[derive(Clone, Copy, PartialEq)]
        enum SpacesOfRuns {
            NoRun,
            All(RasterSpaceIndex),
            Several,
        }
        let spaces = self.tables.spaces();
        // A space whose anchor the compositor animates may yet hold nothing, as when nothing bounds where its content
        // moves, and its content then goes into the space around it.
        let nearest_space_at_or_around_whose_anchor_compositor_does_not_animate = |mut space: RasterSpaceIndex| {
            while is_animated_space(&spaces[space.0]) {
                space = spaces[space.0].parent_index;
            }
            space
        };
        let mut spaces_of_runs_by_effect_index = vec![SpacesOfRuns::NoRun; tree.effect_nodes.len()];
        for run_index in 0..list.run_count() {
            let run = list.run(run_index);
            let mut effect = run.context.effect;
            let space_of_run = nearest_space_at_or_around_whose_anchor_compositor_does_not_animate(
                self.runs_reaching_outside
                    .deepest_space_of_run(tree, &self.tables, run_index, run),
            );
            while !effect.is_none() {
                let spaces_of_runs = &mut spaces_of_runs_by_effect_index[effect.0 as usize];
                *spaces_of_runs = match *spaces_of_runs {
                    SpacesOfRuns::NoRun => SpacesOfRuns::All(space_of_run),
                    SpacesOfRuns::All(space) if space == space_of_run => SpacesOfRuns::All(space),
                    _ => SpacesOfRuns::Several,
                };
                effect = tree.effect_nodes[effect.0 as usize].parent;
            }
        }
        let mut holds_content_of_several_spaces = vec![false; tree.effect_nodes.len()];
        tree.visit_effect_nodes_parents_first(|index| {
            let parent = tree.effect_nodes[index].parent;
            let every_group_effect_around_does =
                parent.is_none() || !groups_runs(parent) || holds_content_of_several_spaces[parent.0 as usize];
            holds_content_of_several_spaces[index] = every_group_effect_around_does
                && may_hold_content_of_several_spaces[index]
                && spaces_of_runs_by_effect_index[index] == SpacesOfRuns::Several;
        });
        if !holds_content_of_several_spaces.contains(&true) {
            self.tables
                .note_effects_composited_around_content_of_several_spaces(Vec::new());
            return;
        }
        self.tables
            .note_effects_composited_around_content_of_several_spaces(holds_content_of_several_spaces.clone());
        self.run_groups = Arc::new(RunGroups::new_leaving_runs_of_effects_ungrouped(
            tree,
            list.runs(),
            &holds_content_of_several_spaces,
        ));
        self.effect_holds_content_of_several_spaces_by_effect_index = holds_content_of_several_spaces;
    }

    // The walk up from an effect stops at the root isolation effect, which holds no reason.
    fn why_compositor_cannot_composite_effect_or_effects_around(
        &self,
        effect: EffectNodeIndex,
    ) -> Option<WhyCompositorCannotCompositeEffect> {
        let tree = self.tree;
        self.why_compositor_cannot_composite_effect_or_effect_around_by_effect_index
            .get_or_init(|| {
                let mut why_by_effect_index: Vec<Option<WhyCompositorCannotCompositeEffect>> =
                    vec![None; tree.effect_nodes.len()];
                tree.visit_effect_nodes_parents_first(|index| {
                    if tree.root_isolation_effect == Some(EffectNodeIndex(index as u32)) {
                        return;
                    }
                    let parent = tree.effect_nodes[index].parent;
                    why_by_effect_index[index] = self.why_compositor_cannot_composite_by_effect_index[index]
                        .or_else(|| why_by_effect_index.get(parent.0 as usize).copied().flatten());
                });
                why_by_effect_index
            })
            .get(effect.0 as usize)
            .copied()
            .flatten()
    }

    fn run_group_of_effect(&self, effect: EffectNodeIndex) -> RunGroupIndex {
        self.run_groups
            .group_of_run(self.first_run_index_by_effect_index[effect.0 as usize])
    }

    fn space_of_run(&self, runs: &RunGroupsPlaced, run_index: usize) -> RasterSpaceIndex {
        runs.groups[self.run_groups.group_of_run(run_index).0].raster_space_index
    }

    fn node_of_effect_holding_content_of_several_spaces(
        runs: &RunGroupsPlaced,
        effect: EffectNodeIndex,
    ) -> Option<NodeOfEffectHoldingContentOfSeveralSpaces> {
        runs.node_by_effect_holding_content_of_several_spaces
            .get(effect.0 as usize)
            .copied()
            .flatten()
    }

    // The space whose raster coordinates `reach_of_effect` is in.
    fn space_of_effect(&self, runs: &RunGroupsPlaced, effect: EffectNodeIndex) -> RasterSpaceIndex {
        match Self::node_of_effect_holding_content_of_several_spaces(runs, effect) {
            Some(node) => node.space,
            None => runs.groups[self.run_group_of_effect(effect).0].raster_space_index,
        }
    }

    fn reach_of_effect(&self, runs: &RunGroupsPlaced, effect: EffectNodeIndex) -> IntRect {
        if let Some(node) = Self::node_of_effect_holding_content_of_several_spaces(runs, effect) {
            return node.reach;
        }
        let (tree, list) = (self.tree, self.list);
        runs.reach_by_effect_index_once_worked_out.get_or_init(|| {
            let mut reach_by_effect_index = vec![IntRect::default(); tree.effect_nodes.len()];
            for (run_index, &reach_of_run) in runs.reach_of_run_by_run_index.iter().enumerate() {
                let mut effect = list.run(run_index).context.effect;
                while !effect.is_none() {
                    if self.run_group_of_effect(effect) == self.run_groups.group_of_run(run_index) {
                        let reach_of_effect = &mut reach_by_effect_index[effect.0 as usize];
                        *reach_of_effect = reach_of_effect.united(reach_of_run);
                    }
                    effect = tree.effect_nodes[effect.0 as usize].parent;
                }
            }
            reach_by_effect_index
        })[effect.0 as usize]
    }

    fn place_run_groups(&self) -> (RunGroupsPlaced, Vec<(AnimatedNode, WhyAnimationHasNoLayer)>) {
        let (tree, list, tables, run_groups) = (self.tree, self.list, &self.tables, &*self.run_groups);
        let spaces = tables.spaces();
        let runs_reaching_outside = &self.runs_reaching_outside;
        let innermost_effect_holding_content_of_several_spaces_at_or_around_by_effect_index =
            self.innermost_effect_holding_content_of_several_spaces_at_or_around_by_effect_index();
        let mut run_blends_with_root_group_by_run_index = vec![false; list.run_count()];
        for record in &list.summary().destination_blending_records {
            let effect = list.run(record.run_index as usize).context.effect;
            if tree.content_under_effect_composites_into_root_group(effect) {
                run_blends_with_root_group_by_run_index[record.run_index as usize] = true;
            }
        }
        let mut reach_of_run_by_run_index = vec![IntRect::default(); list.run_count()];
        let mut run_moves_within_space_by_run_index = vec![false; list.run_count()];

        let mut runs_of_animated_space_by_space_index: Vec<Vec<usize>> = vec![Vec::new(); spaces.len()];
        if self.some_space_is_animated {
            for run_index in 0..list.run_count() {
                let space = tables.deepest_space_of(list.run(run_index).context.spatial);
                if is_animated_space(&spaces[space.0]) {
                    runs_of_animated_space_by_space_index[space.0].push(run_index);
                }
            }
        }
        let mut animated_space_is_decided = vec![false; spaces.len()];
        let mut swept_reach_in_parent_space_by_space_index: Vec<Option<IntRect>> = vec![None; spaces.len()];
        let mut animations_without_layers: Vec<(AnimatedNode, WhyAnimationHasNoLayer)> = Vec::new();

        let mut placed_run_groups: Vec<PlacedRunGroup> =
            Vec::with_capacity(run_groups.first_and_last_run_of_groups().len());
        for &(first_run_index, last_run_index) in run_groups.first_and_last_run_of_groups() {
            if self.some_space_is_animated {
                for run_index in first_run_index as usize..=last_run_index as usize {
                    let space = tables.deepest_space_of(list.run(run_index).context.spatial);
                    if !is_animated_space(&spaces[space.0]) || animated_space_is_decided[space.0] {
                        continue;
                    }
                    animated_space_is_decided[space.0] = true;
                    let canvases_and_videos_placed_before = list
                        .summary()
                        .live_content_records
                        .iter()
                        .filter(|media| matches!(media.content, LiveContent::Canvas(_) | LiveContent::VideoFrame(_)))
                        .filter_map(|media| {
                            let media_run_index = media.record.run_index as usize;
                            let placed_group = placed_run_groups.get(run_groups.group_of_run(media_run_index).0)?;
                            Some((
                                placed_group.raster_space_index,
                                reach_of_run_by_run_index[media_run_index],
                            ))
                        });
                    match self.swept_reach_of_animated_space_in_parent_space(
                        space,
                        &runs_of_animated_space_by_space_index[space.0],
                        canvases_and_videos_placed_before,
                    ) {
                        Ok(swept_reach) => swept_reach_in_parent_space_by_space_index[space.0] = Some(swept_reach),
                        Err(why_no_layer) => {
                            tables.refuse_animated_space(space);
                            animations_without_layers
                                .push((AnimatedNode::Transform(spaces[space.0].anchor), why_no_layer));
                        }
                    }
                }
            }
            placed_run_groups.push({
                let run_indices = first_run_index as usize..=last_run_index as usize;
                let mut raster_space_index = run_indices
                    .clone()
                    .map(|run_index| {
                        runs_reaching_outside.deepest_space_of_run(tree, tables, run_index, list.run(run_index))
                    })
                    .reduce(|first, second| lowest_common_ancestor_space(spaces, first, second))
                    .unwrap_or(RasterSpaceIndex::VIEWPORT);
                let composite_clips = loop {
                    let mut clips_of_runs = run_indices.clone().map(|run_index| {
                        runs_reaching_outside.composite_clips_of_run_in(
                            tree,
                            tables,
                            run_index,
                            list.run(run_index),
                            raster_space_index,
                        )
                    });
                    let first_clips = clips_of_runs.next().flatten();
                    if let Some(first_clips) = first_clips
                        && clips_of_runs.all(|clips| clips.as_deref() == Some(&*first_clips))
                    {
                        break one_composite_clip_per_space_sorted_by_space(&first_clips);
                    }
                    if raster_space_index == RasterSpaceIndex::VIEWPORT {
                        break Vec::new();
                    }
                    raster_space_index = spaces[raster_space_index.0].parent_index;
                };
                let geometry = &self.geometries[raster_space_index.0];
                let mut moves_within_space = false;
                for run_index in run_indices.clone() {
                    let run_moves_within_space =
                        tables.context_moves_within_space(tree, list.run(run_index).context, raster_space_index)
                            || runs_reaching_outside.inline_clips_of_run_move_within_space(
                                tree,
                                tables,
                                run_index,
                                raster_space_index,
                            );
                    run_moves_within_space_by_run_index[run_index] = run_moves_within_space;
                    moves_within_space |= run_moves_within_space;
                }
                let mut reach_of_run_group = IntRect::default();
                let mut reach_read_by_records_blending_with_root_group = Reach::Nothing;
                for run_index in run_indices {
                    let run = list.run(run_index);
                    let ink_bounds_unless_moving_within_space =
                        (!moves_within_space).then(|| run.ink_bounds.to_float());
                    let reach_of_run = geometry.part_of_extent_reached_by(geometry.reach_of_content(
                        &self.filters_and_clips,
                        ink_bounds_unless_moving_within_space,
                        run.context,
                    ));
                    reach_of_run_by_run_index[run_index] = reach_of_run;
                    reach_of_run_group = reach_of_run_group.united(reach_of_run);
                    if run_blends_with_root_group_by_run_index[run_index] {
                        let blend_reads_below_where_it_draws = Reach::Within(reach_of_run);
                        reach_read_by_records_blending_with_root_group =
                            reach_read_by_records_blending_with_root_group.united(blend_reads_below_where_it_draws);
                    }
                }
                let paints_within_root_group = match tree.root_isolation_effect {
                    None => true,
                    Some(root_isolation_effect) => (first_run_index..=last_run_index).any(|run_index| {
                        self.nesting_of_live_effects
                            .is_or_is_inside(list.run(run_index as usize).context.effect, root_isolation_effect)
                    }),
                };
                let effect_of_first_run = list.run(first_run_index as usize).context.effect;
                let inside_effect_holding_content_of_several_spaces =
                    innermost_effect_holding_content_of_several_spaces_at_or_around_by_effect_index
                        .get(effect_of_first_run.0 as usize)
                        .copied()
                        .filter(|effect| !effect.is_none());
                PlacedRunGroup {
                    raster_space_index,
                    composite_clips,
                    reach: reach_of_run_group,
                    moves_within_space,
                    paints_within_root_group,
                    reach_read_by_records_blending_with_root_group,
                    inside_effect_holding_content_of_several_spaces,
                }
            });
        }
        let node_by_effect_holding_content_of_several_spaces = self
            .place_nodes_of_effects_holding_content_of_several_spaces(&placed_run_groups, &reach_of_run_by_run_index);
        let runs = RunGroupsPlaced {
            groups: placed_run_groups,
            reach_of_run_by_run_index,
            run_moves_within_space_by_run_index,
            swept_reach_in_parent_space_by_space_index,
            reach_by_effect_index_once_worked_out: std::cell::OnceCell::new(),
            node_by_effect_holding_content_of_several_spaces,
        };
        (runs, animations_without_layers)
    }

    // Per effect node, the innermost effect at or around it that holds content of several spaces, or none. Empty where
    // no effect holds content of several spaces.
    fn innermost_effect_holding_content_of_several_spaces_at_or_around_by_effect_index(&self) -> Vec<EffectNodeIndex> {
        let tree = self.tree;
        if self.effect_holds_content_of_several_spaces_by_effect_index.is_empty() {
            return Vec::new();
        }
        let mut innermost_by_effect_index = vec![EffectNodeIndex::NONE; tree.effect_nodes.len()];
        tree.visit_effect_nodes_parents_first(|index| {
            let parent = tree.effect_nodes[index].parent;
            innermost_by_effect_index[index] = if self.effect_holds_content_of_several_spaces_by_effect_index[index] {
                EffectNodeIndex(index as u32)
            } else if parent.is_none() {
                EffectNodeIndex::NONE
            } else {
                innermost_by_effect_index[parent.0 as usize]
            };
        });
        innermost_by_effect_index
    }

    // The node of each effect that holds content of several spaces is in the lowest space that holds the run groups of
    // its content, and reaches where that content can show in that space, wherever the spaces inside it take it.
    fn place_nodes_of_effects_holding_content_of_several_spaces(
        &self,
        placed_run_groups: &[PlacedRunGroup],
        reach_of_run_by_run_index: &[IntRect],
    ) -> Vec<Option<NodeOfEffectHoldingContentOfSeveralSpaces>> {
        let (tree, list, tables) = (self.tree, self.list, &self.tables);
        if self.effect_holds_content_of_several_spaces_by_effect_index.is_empty() {
            return Vec::new();
        }
        let space_of_run =
            |run_index: usize| placed_run_groups[self.run_groups.group_of_run(run_index).0].raster_space_index;
        let mut space_by_effect_index: Vec<Option<RasterSpaceIndex>> = vec![None; tree.effect_nodes.len()];
        for run_index in 0..list.run_count() {
            let mut effect = list.run(run_index).context.effect;
            while !effect.is_none() {
                if self.effect_holds_content_of_several_spaces(effect) {
                    let space = &mut space_by_effect_index[effect.0 as usize];
                    *space = Some(match *space {
                        None => space_of_run(run_index),
                        Some(space) => lowest_common_ancestor_space(tables.spaces(), space, space_of_run(run_index)),
                    });
                }
                effect = tree.effect_nodes[effect.0 as usize].parent;
            }
        }
        let mut reach_by_effect_index = vec![IntRect::default(); tree.effect_nodes.len()];
        for (run_index, &reach_of_run) in reach_of_run_by_run_index.iter().enumerate() {
            let run = list.run(run_index);
            if run.ink_bounds.is_empty() {
                continue;
            }
            let mut effect = run.context.effect;
            while !effect.is_none() {
                if let Some(space_of_node) = space_by_effect_index[effect.0 as usize] {
                    let reach_in_space_of_node = if space_of_run(run_index) == space_of_node {
                        reach_of_run
                    } else {
                        let geometry = &self.geometries[space_of_node.0];
                        let drawn_within = (!tables.context_moves_within_space(tree, run.context, space_of_node))
                            .then(|| run.ink_bounds.to_float());
                        geometry.part_of_extent_reached_by(geometry.reach_of_content(
                            &self.filters_and_clips,
                            drawn_within,
                            run.context,
                        ))
                    };
                    let reach = &mut reach_by_effect_index[effect.0 as usize];
                    *reach = reach.united(reach_in_space_of_node);
                }
                effect = tree.effect_nodes[effect.0 as usize].parent;
            }
        }
        space_by_effect_index
            .into_iter()
            .zip(reach_by_effect_index)
            .map(|(space, reach)| space.map(|space| NodeOfEffectHoldingContentOfSeveralSpaces { space, reach }))
            .collect()
    }

    /// Where content of `space`, whose anchor the compositor animates, can show in the raster coordinates of the space
    /// it is in wherever the animation takes it, or why the space holds none of it: content that a frame cannot
    /// composite through the anchor's transform, or whose reach nothing bounds, which would cut every tile around it,
    /// or that can show over a canvas or video painted before it, which would take those from the window server for the
    /// whole animation.
    fn swept_reach_of_animated_space_in_parent_space(
        &self,
        space: RasterSpaceIndex,
        run_indices_of_space: &[usize],
        canvases_and_videos_placed_before: impl Iterator<Item = (RasterSpaceIndex, IntRect)>,
    ) -> Result<IntRect, WhyAnimationHasNoLayer> {
        let (tree, list, tables, geometries) = (self.tree, self.list, &self.tables, self.geometries);
        let filters_and_clips = &self.filters_and_clips;
        let raster_space = tables.spaces()[space.0];
        let RasterSpaceAnchorKind::AnimatedTransform { raster_scale } = raster_space.anchor_kind else {
            unreachable!("only spaces whose anchor the compositor animates are swept");
        };
        if list
            .summary()
            .destination_blending_records
            .iter()
            .any(|record| run_indices_of_space.binary_search(&(record.run_index as usize)).is_ok())
        {
            return Err(WhyAnimationHasNoLayer::ItHoldsRecordsThatBlendWithWhatIsBelowThem);
        }
        for &run_index in run_indices_of_space {
            let mut effect = list.run(run_index).context.effect;
            while !effect.is_none() {
                let effect_node = &tree.effect_nodes[effect.0 as usize];
                let composited_around_space = tables.animated_space_holding_every_run_of_effect(effect) == Some(space)
                    && !tables.moves_with(effect_node.spatial, space);
                if composited_around_space {
                    if effect_reads_below_it(tree, effect) {
                        return Err(WhyAnimationHasNoLayer::ItHoldsAnEffectThatReadsWhatIsBelowIt);
                    }
                    if let Some(why) = self.why_compositor_cannot_composite_by_effect_index[effect.0 as usize] {
                        return Err(WhyAnimationHasNoLayer::InsideAnEffectTheCompositorCannotComposite(why));
                    }
                }
                effect = effect_node.parent;
            }
        }

        let geometry = &geometries[space.0];
        // Content that moves within the space, as a sample of an animation inside it moves it or spreads it, reaches
        // wherever its clips fixed in the space let it.
        let content_reach_in_space = run_indices_of_space.iter().fold(Reach::Nothing, |reach, &run_index| {
            let run = list.run(run_index);
            if run.ink_bounds.is_empty() {
                return reach;
            }
            let drawn_within =
                (!tables.context_moves_within_space(tree, run.context, space)).then(|| run.ink_bounds.to_float());
            reach.united(geometry.reach_of_content(filters_and_clips, drawn_within, run.context))
        });
        let content_rect_in_anchor_space = match content_reach_in_space {
            Reach::Nothing => return Ok(IntRect::default()),
            Reach::Anywhere => return Err(WhyAnimationHasNoLayer::NothingBoundsWhereItMoves),
            Reach::Within(rect) => libgfx_rust::FloatRect::new(
                rect.x as f32 / raster_scale,
                rect.y as f32 / raster_scale,
                rect.width as f32 / raster_scale,
                rect.height as f32 / raster_scale,
            ),
        };

        let parent_space = raster_space.parent_index;
        let parent_geometry = &geometries[parent_space.0];
        let parent_of_anchor = tree.spatial_nodes[raster_space.anchor.0 as usize].parent;
        let reach_within_clips_fixed_in_parent_space =
            run_indices_of_space.iter().fold(Reach::Nothing, |reach, &run_index| {
                reach.united(parent_geometry.reach_of_content(filters_and_clips, None, list.run(run_index).context))
            });
        let swept_bounds_in_parent_space = (!tables.spatial_moves_within_space(parent_of_anchor, parent_space))
            .then(|| {
                crate::display_list::animated_transform_spaces::swept_bounds_in_parent(
                    tree,
                    raster_space.anchor,
                    content_rect_in_anchor_space,
                )
            })
            .flatten()
            .map(|bounds| parent_geometry.raster.world.map_rect_to_root(parent_of_anchor, bounds))
            .filter(|bounds| is_finite_rect(*bounds));
        let swept_reach = match swept_bounds_in_parent_space {
            Some(bounds) => {
                let margin = crate::display_list::placement::ANTIALIASING_MARGIN_IN_DEVICE_PIXELS;
                let swept_rect =
                    enclosing_int_rect_within_extent_limit(bounds).inflated_edges(margin, margin, margin, margin);
                reach_within_clips_fixed_in_parent_space.limited_to(swept_rect)
            }
            None => reach_within_clips_fixed_in_parent_space,
        };
        if swept_reach == Reach::Anywhere {
            return Err(WhyAnimationHasNoLayer::NothingBoundsWhereItMoves);
        }
        let swept_reach = parent_geometry.part_of_extent_reached_by(swept_reach);

        for (space_of_media, reach_of_media) in canvases_and_videos_placed_before {
            if space_of_media != parent_space || !reach_of_media.intersected(swept_reach).is_empty() {
                return Err(WhyAnimationHasNoLayer::ItCanShowOverAnEarlierCanvasOrVideo);
            }
        }
        Ok(swept_reach)
    }

    fn place_media(
        &self,
        runs: &RunGroupsPlaced,
        most_canvas_video_and_nested_context_layers: Option<usize>,
    ) -> (Vec<PlacedMedia>, Vec<(LiveContent, WhyMediaHasNoLayer)>) {
        let (tree, list, run_groups) = (self.tree, self.list, &*self.run_groups);
        let mut placed_media: Vec<PlacedMedia> = Vec::new();
        let mut media_without_layers = Vec::new();
        let spatial_node_is_under_transform_animation_leaving_plane =
            spatial_nodes_under_transform_animations(tree, |node| tree.transform_animation_of_node_leaves_plane(node));
        for live_content_record in &list.summary().live_content_records {
            let content = live_content_record.content;
            if matches!(content, LiveContent::ImageFrame(_)) {
                continue;
            }
            let record: IndexedRecord = live_content_record.record;
            let (run_index, record_position) = (record.run_index, record.position);
            let why_no_layer = if run_groups.run_is_sorted_by_depth(run_index as usize) {
                Some(WhyMediaHasNoLayer::SortedByDepthInA3DSortingContext)
            } else {
                self.why_media_has_no_layer(
                    runs,
                    content,
                    record,
                    spatial_node_is_under_transform_animation_leaving_plane
                        .get(list.run(run_index as usize).context.spatial.0 as usize)
                        .copied()
                        .unwrap_or(false),
                )
            };
            match why_no_layer {
                None => {
                    let record_span = list.span_of_run(run_index as usize);
                    debug_assert_eq!(
                        record_position, record_span.begin,
                        "live content with a layer is alone in its run"
                    );
                    let reach = runs.reach_of_run_by_run_index[run_index as usize];
                    placed_media.push(PlacedMedia {
                        content,
                        ordinal: 0,
                        record_span,
                        run_index: RunIndexInList(run_index),
                        reach,
                        cuts_band: self.record_painted_after_can_show_over(runs, run_index as usize, reach),
                    });
                }
                Some(why_no_layer) => media_without_layers.push((content, why_no_layer)),
            }
        }
        // Past the most layers that carets and scroll bars get, carets keep theirs before scroll bars, and the scroll bars
        // of the viewport before those of other scrollers.
        let order_among_others_of_its_kind = |media: &PlacedMedia| match media.content {
            LiveContent::ScrollBar { scroll_node, .. } => {
                if compositor_scroll_record_of(tree, scroll_node).is_some_and(|record| record.is_viewport) {
                    1
                } else {
                    2
                }
            }
            _ => 0,
        };
        placed_media.sort_by_key(order_among_others_of_its_kind);
        let mut layer_count_of_live_content_drawn_by_its_record = 0;
        let mut layer_count_of_canvases_videos_and_nested_contexts = 0;
        placed_media.retain(|media| {
            let (layer_count, maximum_layer_count) = if media.content.is_drawn_by_its_record() {
                (
                    &mut layer_count_of_live_content_drawn_by_its_record,
                    Some(MAXIMUM_LAYER_COUNT_OF_LIVE_CONTENT_DRAWN_BY_ITS_RECORD),
                )
            } else {
                (
                    &mut layer_count_of_canvases_videos_and_nested_contexts,
                    most_canvas_video_and_nested_context_layers,
                )
            };
            *layer_count += 1;
            let gets_layer = maximum_layer_count.is_none_or(|maximum| *layer_count <= maximum);
            if !gets_layer {
                media_without_layers.push((media.content, WhyMediaHasNoLayer::MoreMediaThanLayersFor));
            }
            gets_layer
        });
        placed_media.sort_by_key(|media| media.record_span.begin);
        let mut drawings_painted_before_by_content: HashMap<LiveContent, u32> = HashMap::new();
        for media in &mut placed_media {
            let drawings_painted_before = drawings_painted_before_by_content.entry(media.content).or_insert(0);
            media.ordinal = *drawings_painted_before;
            *drawings_painted_before += 1;
        }
        (placed_media, media_without_layers)
    }

    // Under a transform animation that leaves the plane, a sample can draw the media through a matrix that is not 2D
    // affine, whatever the tree holds. A caret or a scroll bar does not get the effects around it composited, as other
    // live content does: it has no layer inside an effect that a replay draws into a layer of its own.
    fn why_media_has_no_layer(
        &self,
        runs: &RunGroupsPlaced,
        content: LiveContent,
        record: IndexedRecord,
        is_under_transform_animation_leaving_plane: bool,
    ) -> Option<WhyMediaHasNoLayer> {
        let run_index = record.run_index as usize;
        let (header, _) = self.list.record_at(record.position);
        // Live content nested in another command is noted at the top-level record around it.
        if !header.command_type.is_alone_in_its_run() {
            return Some(WhyMediaHasNoLayer::DrawnInsideAnotherCommand);
        }
        let run = self.list.run(run_index);
        let geometry = &self.geometries[self.space_of_run(runs, run_index).0].raster;
        if is_under_transform_animation_leaving_plane || !geometry.world.draw_matrix(run.context.spatial).is_2d_affine()
        {
            return Some(WhyMediaHasNoLayer::UnderTransformThatIsNot2DAffine);
        }
        if content.is_drawn_by_its_record() {
            if !self
                .tree
                .content_under_effect_composites_into_root_group(run.context.effect)
            {
                return Some(WhyMediaHasNoLayer::InsideAnEffectThatPushesALayerAtReplay);
            }
            // Its reach is then wherever its fixed clips let it show.
            if runs.run_moves_within_space_by_run_index[run_index] {
                return Some(WhyMediaHasNoLayer::MovesWithinItsSpace);
            }
        } else if let Some(why) = self.why_compositor_cannot_composite_effect_or_effects_around(run.context.effect) {
            return Some(WhyMediaHasNoLayer::InsideAnEffectTheCompositorCannotComposite(why));
        }
        if runs.reach_of_run_by_run_index[run_index].is_empty() {
            return Some(WhyMediaHasNoLayer::DrawsNothingVisible);
        }
        None
    }

    // Whether a record painted after the media alone in run `run_index`, which can show within `reach`, can show where
    // the media can, so that the media cuts the tiles of its band that it touches. Records of other spaces go into
    // other bands. A record of a run on the same spatial node can show where its bounds meet those of the media, and
    // one of a run on another node wherever its run can.
    fn record_painted_after_can_show_over(&self, runs: &RunGroupsPlaced, run_index: usize, reach: IntRect) -> bool {
        let list = self.list;
        let space = self.space_of_run(runs, run_index);
        let spatial = list.run(run_index).context.spatial;
        let (header, payload) = list.record_at(list.span_of_run(run_index).begin);
        let Some(bounds) = bounding_rect_computed_from_payload(&header, payload) else {
            return true;
        };
        let margin = ANTIALIASING_MARGIN_IN_DEVICE_PIXELS;
        let bounds = bounds.inflated_edges(margin, margin, margin, margin);
        (run_index + 1..list.run_count()).any(|later_run_index| {
            if self.space_of_run(runs, later_run_index) != space
                || runs.reach_of_run_by_run_index[later_run_index]
                    .intersected(reach)
                    .is_empty()
            {
                return false;
            }
            if list.run(later_run_index).context.spatial != spatial {
                return true;
            }
            let (chunk_index, run_index_in_chunk) = list.chunk_of_run(later_run_index);
            let chunk = list.chunk(chunk_index);
            let table = chunk.record_table();
            table
                .records_in(table.record_indices_of_run(RunIndexInChunk(run_index_in_chunk as u32)))
                .iter()
                .any(|record| {
                    let (header, payload) = chunk.record_at(record.offset);
                    !record.bounding_rect.intersected(bounds).is_empty()
                        && record_can_paint_within(&header, payload, bounds)
                })
        })
    }

    fn effects_composited_regardless_of_layer_content(
        &self,
        runs: &RunGroupsPlaced,
        placed_media: &[PlacedMedia],
        animations_without_layers: &mut Vec<(AnimatedNode, WhyAnimationHasNoLayer)>,
    ) -> WhyEffectsAreCompositedRegardlessOfLayerContent {
        let (tree, list) = (self.tree, self.list);
        let spaces = self.tables.spaces();
        let mut why_composited_regardless_of_layer_content = WhyEffectsAreCompositedRegardlessOfLayerContent {
            why_by_effect_index: vec![None; tree.effect_nodes.len()],
            composited_effects: Vec::new(),
        };
        // Every group effect around one of these holds content of several spaces too, and comes before it.
        for &effect in &self.effects_in_paint_order_with_outer_before_inner {
            if self.effect_holds_content_of_several_spaces(effect) {
                why_composited_regardless_of_layer_content
                    .set(effect, WhyEffectIsComposited::ItHoldsContentOfSeveralSpaces);
            }
        }
        for media in placed_media {
            why_composited_regardless_of_layer_content.mark_effects_around_as_composited(
                tree,
                list.run(media.run_index.0 as usize).context.effect,
                WhyEffectIsComposited::ItHoldsMedia(media.content),
            );
        }

        let effect_reads_what_is_below_it = |effect: EffectNodeIndex| {
            self.backdrop_reader_by_effect_index[effect.0 as usize].is_some() || effect_reads_below_it(tree, effect)
        };
        let canvas_or_video_painted_before_can_show_under =
            |first_run_index: usize, raster_space_index: RasterSpaceIndex, reach: IntRect| {
                placed_media.iter().any(|media| {
                    matches!(media.content, LiveContent::Canvas(_) | LiveContent::VideoFrame(_))
                        && (media.run_index.0 as usize) < first_run_index
                        && (self.space_of_run(runs, media.run_index.0 as usize) != raster_space_index
                            || !media.reach.intersected(reach).is_empty())
                })
            };
        let mut animated_nodes_with_layers_count = 0;

        let space_holds_placed_content_by_space_index = std::cell::OnceCell::new();
        let space_holds_placed_content = |space: RasterSpaceIndex| {
            space_holds_placed_content_by_space_index.get_or_init(|| {
                let mut holds_by_space_index = vec![false; spaces.len()];
                for run_group in &runs.groups {
                    holds_by_space_index[run_group.raster_space_index.0] = true;
                }
                holds_by_space_index
            })[space.0]
        };
        if self.some_space_is_animated {
            for (index, effect_node) in tree.effect_nodes.iter().enumerate() {
                let effect = EffectNodeIndex(index as u32);
                let Some(space) = self.tables.animated_space_holding_every_run_of_effect(effect) else {
                    continue;
                };
                let applies_over_animated_transform = runs.swept_reach_in_parent_space_by_space_index[space.0]
                    .is_some()
                    && !self.tables.moves_with(effect_node.spatial, space)
                    && space_holds_placed_content(space);
                if !applies_over_animated_transform || !tree.effect_pushes_layer_at_replay(effect) {
                    continue;
                }
                why_composited_regardless_of_layer_content.set_unless_composited(
                    effect,
                    WhyEffectIsComposited::ItAppliesOverATransformTheCompositorAnimates,
                );
                why_composited_regardless_of_layer_content.mark_effects_around_as_composited(
                    tree,
                    effect_node.parent,
                    WhyEffectIsComposited::ItHoldsAnEffectTheCompositorComposites,
                );
            }
        }

        let later_backdrop_readers_once_worked_out = std::cell::OnceCell::new();
        let later_backdrop_readers = || {
            later_backdrop_readers_once_worked_out
                .get_or_init(|| self.backdrop_readers_an_animation_may_cut(runs, placed_media))
        };
        let what_effects_hold_once_worked_out = std::cell::OnceCell::new();
        let what_effects_hold = || {
            what_effects_hold_once_worked_out.get_or_init(|| {
                let mut runs_holding_records_blending_with_what_is_below_them = vec![false; list.run_count()];
                for record in &list.summary().destination_blending_records {
                    runs_holding_records_blending_with_what_is_below_them[record.run_index as usize] = true;
                }
                let mut effect_reads_what_is_below_it_by_effect_index = vec![false; tree.effect_nodes.len()];
                for &effect in &self.effects_in_paint_order_with_outer_before_inner {
                    effect_reads_what_is_below_it_by_effect_index[effect.0 as usize] =
                        effect_reads_what_is_below_it(effect);
                }
                let mut marks = [
                    effects_of_marked_runs(list, tree, |run_index| {
                        self.run_groups.run_is_sorted_by_depth(run_index)
                    }),
                    effects_of_marked_runs(list, tree, |run_index| {
                        runs_holding_records_blending_with_what_is_below_them[run_index]
                    }),
                    effect_reads_what_is_below_it_by_effect_index,
                ];
                mark_effects_around_marked_effects(tree, &mut marks);
                let [
                    run_sorted_by_depth_by_effect_index,
                    records_blending_with_what_is_below_them_by_effect_index,
                    effect_reading_what_is_below_it_by_effect_index,
                ] = marks;
                WhatEffectsHold {
                    run_sorted_by_depth_by_effect_index,
                    records_blending_with_what_is_below_them_by_effect_index,
                    effect_reading_what_is_below_it_by_effect_index,
                }
            })
        };
        for &effect in &self.effects_in_paint_order_with_outer_before_inner {
            if !tree.effect_opacity_or_filter_is_animated(effect) {
                continue;
            }
            let filter_is_animated = tree.effect_filter_is_animated(effect);
            let first_run_index = self.first_run_index_by_effect_index[effect.0 as usize];
            if tree.root_isolation_effect == Some(effect)
                || !matches!(tree.effect_nodes[effect.0 as usize].data, EffectNodeData::Effects(_))
            {
                continue;
            }
            let why_no_layer = (|| {
                if filter_is_animated && filter_may_spread_at_some_sample(tree, effect) {
                    return Some(WhyAnimationHasNoLayer::ItsFilterSpreadsWhatItFilters);
                }
                if let Some(why) = self.why_compositor_cannot_composite_effect_or_effects_around(effect) {
                    return Some(WhyAnimationHasNoLayer::InsideAnEffectTheCompositorCannotComposite(why));
                }
                let what_effects_hold = what_effects_hold();
                if what_effects_hold.run_sorted_by_depth_by_effect_index[effect.0 as usize] {
                    return Some(WhyAnimationHasNoLayer::SortedByDepthInA3DSortingContext);
                }
                if what_effects_hold.records_blending_with_what_is_below_them_by_effect_index[effect.0 as usize] {
                    return Some(WhyAnimationHasNoLayer::ItHoldsRecordsThatBlendWithWhatIsBelowThem);
                }
                if what_effects_hold.effect_reading_what_is_below_it_by_effect_index[effect.0 as usize] {
                    return Some(WhyAnimationHasNoLayer::ItHoldsAnEffectThatReadsWhatIsBelowIt);
                }
                if canvas_or_video_painted_before_can_show_under(
                    first_run_index,
                    self.space_of_effect(runs, effect),
                    self.reach_of_effect(runs, effect),
                ) {
                    return Some(WhyAnimationHasNoLayer::ItCanShowOverAnEarlierCanvasOrVideo);
                }
                if animated_nodes_with_layers_count == MAXIMUM_ANIMATED_NODES_WITH_LAYERS {
                    return Some(WhyAnimationHasNoLayer::MoreAnimationsThanLayersFor);
                }
                // NB: This scans the backdrop readers, so it comes once the animation could still get a layer.
                if self.later_effect_reads_as_backdrop(
                    runs,
                    effect,
                    later_backdrop_readers(),
                    &why_composited_regardless_of_layer_content.composited_effects,
                ) {
                    return Some(WhyAnimationHasNoLayer::ALaterEffectReadsItAsBackdrop);
                }
                None
            })();
            if let Some(why_no_layer) = why_no_layer {
                animations_without_layers.push((AnimatedNode::Effect(effect), why_no_layer));
                continue;
            }
            animated_nodes_with_layers_count += 1;
            why_composited_regardless_of_layer_content
                .set_unless_composited(effect, WhyEffectIsComposited::TheCompositorAnimatesIt);
            why_composited_regardless_of_layer_content.mark_effects_around_as_composited(
                tree,
                tree.effect_nodes[effect.0 as usize].parent,
                WhyEffectIsComposited::ItHoldsAnEffectTheCompositorComposites,
            );
        }
        why_composited_regardless_of_layer_content
    }

    /// Where the effect reads the backdrop it filters or blends with, and whether its backdrop filter reads nothing
    /// past the edge it mirrors.
    fn backdrop_read_reach(
        &self,
        runs: &RunGroupsPlaced,
        effect: EffectNodeIndex,
        backdrop_reader: BackdropReader,
    ) -> (Reach<IntRect>, bool) {
        let tree = self.tree;
        let run_group = &runs.groups[self.run_group_of_effect(effect).0];
        let geometry = &self.geometries[run_group.raster_space_index.0];
        let reach_of_reading_effect = self.reach_of_effect(runs, effect);
        match backdrop_reader.effect_with_backdrop_filter {
            Some(effect_with_backdrop_filter) => {
                let effect_node = &tree.effect_nodes[effect_with_backdrop_filter.0 as usize];
                let EffectNodeData::Effects(effects) = &effect_node.data else {
                    unreachable!("a backdrop filter belongs to an effects node");
                };
                let backdrop_filter = effects
                    .backdrop_filter
                    .as_ref()
                    .expect("a reader with a backdrop filter has one");
                // Where a transform the compositor animates moves or scales the backdrop filter within the
                // space, a sample can read anywhere.
                let backdrop_read_reach = if self
                    .tables
                    .animation_samples_move_spatial_within_space(effect_node.spatial, run_group.raster_space_index)
                {
                    Reach::Anywhere
                } else if run_group.moves_within_space {
                    filter_read_reach_in_device_pixels(
                        &backdrop_filter.filter,
                        &geometry.raster.world.draw_matrix(effect_node.spatial),
                    )
                    .map_or(Reach::Anywhere, |read_reach_in_device_pixels| {
                        Reach::Within(run_group.reach.inflated_edges(
                            read_reach_in_device_pixels,
                            read_reach_in_device_pixels,
                            read_reach_in_device_pixels,
                            read_reach_in_device_pixels,
                        ))
                    })
                } else {
                    backdrop_filter_read_reach(tree, &geometry.raster, effect_with_backdrop_filter)
                };
                (
                    backdrop_read_reach,
                    backdrop_filter_reads_nothing_past_mirrored_edge(&backdrop_filter.filter),
                )
            }
            None => (Reach::Within(reach_of_reading_effect), true),
        }
    }

    /// The effect whose group the effect reads the backdrop within, or None where it reads the root group.
    fn backdrop_root_of_reader(&self, effect: EffectNodeIndex) -> Option<EffectNodeIndex> {
        let tree = self.tree;
        let reading_effect_node = &tree.effect_nodes[effect.0 as usize];
        if reading_effect_node.parent.is_none() {
            return None;
        }
        tree.nearest_layer_at_or_above(reading_effect_node.parent)
            .filter(|root| tree.root_isolation_effect != Some(*root))
    }

    // The backdrop readers that a layer of an animation could get composited: those that read within a group of their
    // own, and that nothing but a composited effect painted before them would get composited, with the tiles they read.
    fn backdrop_readers_an_animation_may_cut(
        &self,
        runs: &RunGroupsPlaced,
        placed_media: &[PlacedMedia],
    ) -> Vec<BackdropReaderAnAnimationMayCut> {
        let list = self.list;
        let mut readers = Vec::new();
        for &reader in &self.effects_in_paint_order_with_outer_before_inner {
            let Some(backdrop_reader) = self.backdrop_reader_by_effect_index[reader.0 as usize] else {
                continue;
            };
            let Some(backdrop_root) = self.backdrop_root_of_reader(reader) else {
                continue;
            };
            if self.effect_holds_content_of_several_spaces(backdrop_root) {
                continue;
            }
            let (backdrop_read_reach, reads_nothing_past_mirrored_edge) =
                self.backdrop_read_reach(runs, reader, backdrop_reader);
            if !reads_nothing_past_mirrored_edge {
                continue;
            }
            let read_tiles = match backdrop_read_reach {
                Reach::Within(rect) => Reach::Within(CellRange::touched_by(rect).rect()),
                backdrop_read_reach => backdrop_read_reach,
            };
            let first_run_index = self.first_run_index_by_effect_index[reader.0 as usize];
            let read_tiles_cut_by_earlier_media = placed_media.iter().any(|media| {
                let effect_of_media = list.run(media.run_index.0 as usize).context.effect;
                (media.run_index.0 as usize) < first_run_index
                    && self
                        .nesting_of_live_effects
                        .is_or_is_inside(effect_of_media, backdrop_root)
                    && read_tiles.meets(Reach::Within(media.reach))
            });
            if read_tiles_cut_by_earlier_media {
                continue;
            }
            readers.push(BackdropReaderAnAnimationMayCut {
                reader,
                backdrop_root,
                first_run_index,
                read_tiles,
            });
        }
        readers
    }

    // Whether a composited effect cuts the tiles the reader reads, so that the reader reads what its own layer does not
    // hold, as `note_backdrop_readers` works out.
    fn composited_effect_cuts_read_tiles_of(
        &self,
        runs: &RunGroupsPlaced,
        composited: EffectNodeIndex,
        candidate: &BackdropReaderAnAnimationMayCut,
    ) -> bool {
        if composited == candidate.reader
            || self.first_run_index_by_effect_index[composited.0 as usize] >= candidate.first_run_index
            || !self
                .nesting_of_live_effects
                .is_strictly_inside(composited, candidate.backdrop_root)
            || self
                .nesting_of_live_effects
                .is_strictly_inside(candidate.reader, composited)
        {
            return false;
        }
        let mut reach = self.reach_of_effect(runs, composited);
        if let Some(backdrop_reader) = self.backdrop_reader_by_effect_index[composited.0 as usize]
            && let Reach::Within(rect) = self.backdrop_read_reach(runs, composited, backdrop_reader).0
        {
            reach = reach.united(rect);
        }
        candidate.read_tiles.meets(Reach::Within(reach))
    }

    // A layer of an animation of the effect composites the effect and every effect around it that pushes a layer. A
    // backdrop reader painted after the effect, and outside it, that reads where one of those reaches within its own
    // group would then be composited for reading what its own layer does not hold, and so would every effect around it,
    // as `note_backdrop_readers` works out. A reader composited already, or that an effect composited already cuts, is
    // composited anyway.
    fn later_effect_reads_as_backdrop(
        &self,
        runs: &RunGroupsPlaced,
        effect: EffectNodeIndex,
        later_backdrop_readers: &[BackdropReaderAnAnimationMayCut],
        composited_effects: &[EffectNodeIndex],
    ) -> bool {
        let tree = self.tree;
        let first_run_index = self.first_run_index_by_effect_index[effect.0 as usize];
        later_backdrop_readers.iter().any(|candidate| {
            if candidate.first_run_index <= first_run_index
                || self.nesting_of_live_effects.is_or_is_inside(candidate.reader, effect)
                || composited_effects.contains(&candidate.reader)
                || composited_effects
                    .iter()
                    .any(|&composited| self.composited_effect_cuts_read_tiles_of(runs, composited, candidate))
            {
                return false;
            }
            let mut composited = effect;
            while !composited.is_none() && tree.root_isolation_effect != Some(composited) {
                if (composited == effect || tree.effect_pushes_layer_at_replay(composited))
                    && self.composited_effect_cuts_read_tiles_of(runs, composited, candidate)
                {
                    return true;
                }
                composited = tree.effect_nodes[composited.0 as usize].parent;
            }
            false
        })
    }

    // What each backdrop reader reads, in paint order, and whether it reads the root group. An effect that reads what
    // its own layer does not hold is composited.
    fn note_backdrop_readers(
        &self,
        runs: &RunGroupsPlaced,
        placed_media: &[PlacedMedia],
        why_composited_regardless_of_layer_content: &mut WhyEffectsAreCompositedRegardlessOfLayerContent,
    ) -> (Vec<Reach<IntRect>>, Vec<EffectNodeIndex>, Vec<bool>) {
        let (tree, list) = (self.tree, self.list);
        let mut backdrop_read_reach_by_effect_index = vec![Reach::Nothing; tree.effect_nodes.len()];
        let mut backdrop_readers_in_paint_order = Vec::new();
        // Empty until an effect reads the root group.
        let mut reads_root_group_by_effect_index: Vec<bool> = Vec::new();
        for &effect in &self.effects_in_paint_order_with_outer_before_inner {
            let Some(backdrop_reader) = self.backdrop_reader_by_effect_index[effect.0 as usize] else {
                continue;
            };
            let (backdrop_read_reach, reads_nothing_past_mirrored_edge) =
                self.backdrop_read_reach(runs, effect, backdrop_reader);
            backdrop_read_reach_by_effect_index[effect.0 as usize] = backdrop_read_reach;
            let reading_effect_node = &tree.effect_nodes[effect.0 as usize];
            let backdrop_root = self.backdrop_root_of_reader(effect);
            let why_composited = if !reads_nothing_past_mirrored_edge {
                Some(WhyEffectIsComposited::ItsBackdropFilterReadsPastTheEdgeOfTheFrame)
            } else if backdrop_root.is_some_and(|root| self.effect_holds_content_of_several_spaces(root)) {
                // Its backdrop is in nodes of several spaces.
                Some(WhyEffectIsComposited::ItReadsWhatItsOwnLayerDoesNotHold)
            } else if let Some(backdrop_root) = backdrop_root {
                let backdrop_read_tiles = match backdrop_read_reach {
                    Reach::Within(rect) => Reach::Within(CellRange::touched_by(rect).rect()),
                    backdrop_read_reach => backdrop_read_reach,
                };
                let read_tiles_cut_by_earlier_composited_effect = why_composited_regardless_of_layer_content
                    .composited_effects
                    .iter()
                    .any(|&other_effect| {
                        other_effect != effect
                            && self.first_run_index_by_effect_index[other_effect.0 as usize]
                                < self.first_run_index_by_effect_index[effect.0 as usize]
                            && self
                                .nesting_of_live_effects
                                .is_strictly_inside(other_effect, backdrop_root)
                            && !self.nesting_of_live_effects.is_strictly_inside(effect, other_effect)
                            && backdrop_read_tiles.meets(Reach::Within(
                                self.reach_of_effect(runs, other_effect).united(
                                    match backdrop_read_reach_by_effect_index[other_effect.0 as usize] {
                                        Reach::Within(rect) => rect,
                                        _ => IntRect::default(),
                                    },
                                ),
                            ))
                    });
                let read_tiles_cut_by_earlier_media = placed_media.iter().any(|media| {
                    let effect_of_media = list.run(media.run_index.0 as usize).context.effect;
                    (media.run_index.0 as usize) < self.first_run_index_by_effect_index[effect.0 as usize]
                        && self
                            .nesting_of_live_effects
                            .is_or_is_inside(effect_of_media, backdrop_root)
                        && backdrop_read_tiles.meets(Reach::Within(media.reach))
                });
                (read_tiles_cut_by_earlier_composited_effect || read_tiles_cut_by_earlier_media)
                    .then_some(WhyEffectIsComposited::ItReadsWhatItsOwnLayerDoesNotHold)
            } else {
                None
            };
            backdrop_readers_in_paint_order.push(effect);
            if backdrop_root.is_none() {
                reads_root_group_by_effect_index.resize(tree.effect_nodes.len(), false);
                reads_root_group_by_effect_index[effect.0 as usize] = true;
            }
            let Some(why_composited) = why_composited else {
                continue;
            };
            why_composited_regardless_of_layer_content.set(effect, why_composited);
            why_composited_regardless_of_layer_content.mark_effects_around_as_composited(
                tree,
                reading_effect_node.parent,
                WhyEffectIsComposited::ItHoldsAnEffectTheCompositorComposites,
            );
        }
        (
            backdrop_read_reach_by_effect_index,
            backdrop_readers_in_paint_order,
            reads_root_group_by_effect_index,
        )
    }

    fn place_effects(
        &self,
        runs: &RunGroupsPlaced,
        why_composited_regardless_of_layer_content: &WhyEffectsAreCompositedRegardlessOfLayerContent,
        backdrop_read_reach_by_effect_index: &[Reach<IntRect>],
        reads_root_group_by_effect_index: &[bool],
    ) -> (Vec<PlacedEffect>, Vec<Option<PlacedEffectIndex>>) {
        let tree = self.tree;
        // Effects come after the effects around them, so the innermost placed effect at or around each is known by
        // then.
        let mut innermost_placed_effect_at_or_around_by_effect_index: Vec<Option<PlacedEffectIndex>> =
            vec![None; tree.effect_nodes.len()];
        let mut placed_effects: Vec<PlacedEffect> = Vec::new();
        for &effect in &self.effects_in_paint_order_with_outer_before_inner {
            let parent = tree.effect_nodes[effect.0 as usize].parent;
            let innermost_placed_effect_around = innermost_placed_effect_at_or_around_by_effect_index
                .get(parent.0 as usize)
                .copied()
                .flatten();
            innermost_placed_effect_at_or_around_by_effect_index[effect.0 as usize] = innermost_placed_effect_around;
            let why_composited_regardless_of_layer_content =
                why_composited_regardless_of_layer_content.why_by_effect_index[effect.0 as usize];
            let reads_root_group = reads_root_group_by_effect_index
                .get(effect.0 as usize)
                .copied()
                .unwrap_or(false);
            if why_composited_regardless_of_layer_content.is_none() && !reads_root_group {
                continue;
            }
            innermost_placed_effect_at_or_around_by_effect_index[effect.0 as usize] =
                Some(PlacedEffectIndex(placed_effects.len()));
            placed_effects.push(PlacedEffect {
                effect,
                placed_effect_around_index: innermost_placed_effect_around,
                reach: self.reach_of_effect(runs, effect),
                backdrop_read_reach: backdrop_read_reach_by_effect_index[effect.0 as usize],
                why_composited_regardless_of_layer_content,
                space_of_node_holding_content_of_several_spaces:
                    Self::node_of_effect_holding_content_of_several_spaces(runs, effect).map(|node| node.space),
            });
        }
        (placed_effects, innermost_placed_effect_at_or_around_by_effect_index)
    }

    fn place_record_stretches(
        &self,
        runs: &RunGroupsPlaced,
        placed_media: &[PlacedMedia],
        placed_effects: &[PlacedEffect],
        innermost_placed_effect_at_or_around_by_effect_index: &[Option<PlacedEffectIndex>],
    ) -> (Vec<PlacedRecordStretch>, Vec<bool>) {
        let list = self.list;
        let innermost_placed_effect_of_run = |run_index: usize| {
            innermost_placed_effect_at_or_around_by_effect_index
                .get(list.run(run_index).context.effect.0 as usize)
                .copied()
                .flatten()
        };

        let mut record_stretches: Vec<PlacedRecordStretch> = Vec::with_capacity(runs.groups.len());
        let mut remaining_placed_media = placed_media.iter().enumerate().peekable();
        let mut tiles_hold_content_moving_within_space_by_space_index = vec![false; self.tables.spaces().len()];
        for run_index in 0..list.run_count() {
            let run_group_index = self.run_groups.group_of_run(run_index);
            let innermost_placed_effect = innermost_placed_effect_of_run(run_index);
            if let Some((placed_media_index, media)) =
                remaining_placed_media.next_if(|(_, media)| media.run_index.0 as usize == run_index)
            {
                record_stretches.push(PlacedRecordStretch {
                    record_span: media.record_span,
                    run_group_index,
                    reach: media.reach,
                    placed_media_index: Some(PlacedMediaIndex(placed_media_index)),
                    placed_effect_index: innermost_placed_effect,
                });
                continue;
            }
            if runs.run_moves_within_space_by_run_index[run_index] {
                tiles_hold_content_moving_within_space_by_space_index[self.space_of_run(runs, run_index).0] = true;
            }
            let run_span = list.span_of_run(run_index);
            let run_draws_nothing = list.run(run_index).ink_bounds.is_empty();
            // The reach of an effect holding content of several spaces is in the space of its node.
            let reach_of_run_in_stretch = match innermost_placed_effect {
                _ if run_draws_nothing => IntRect::default(),
                Some(placed_effect_index)
                    if placed_effects[placed_effect_index.0]
                        .space_of_node_holding_content_of_several_spaces
                        .is_none() =>
                {
                    placed_effects[placed_effect_index.0].reach
                }
                _ => runs.reach_of_run_by_run_index[run_index],
            };
            match record_stretches.last_mut() {
                Some(last)
                    if last.run_group_index == run_group_index
                        && last.placed_effect_index == innermost_placed_effect
                        && last.placed_media_index.is_none() =>
                {
                    last.record_span.end = run_span.end;
                    last.reach = last.reach.united(reach_of_run_in_stretch);
                }
                _ => record_stretches.push(PlacedRecordStretch {
                    record_span: run_span,
                    run_group_index,
                    reach: reach_of_run_in_stretch,
                    placed_media_index: None,
                    placed_effect_index: innermost_placed_effect,
                }),
            }
        }
        (record_stretches, tiles_hold_content_moving_within_space_by_space_index)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
struct BandBeingBuiltIndex(usize);

struct BandBeingBuilt {
    placement_node_index: PlacementNodeIndex,
    barriers: Vec<BarrierBeingBuilt>,
    own_record_stretch_indices: Vec<RecordStretchIndex>,
    first_record_stretch_index: RecordStretchIndex,
    root_group_reach_so_far: Reach<IntRect>,
}

#[derive(Clone, Copy)]
enum BarrierBeingBuilt {
    Media {
        record_stretch_index: RecordStretchIndex,
    },
    NestedNodeBand {
        band_being_built_index: BandBeingBuiltIndex,
    },
}

// Calls `for_layer_and_part_of_reach` with each layer that records of a band at `record_position` go into, and the part
// of `reach` they go into it in: per tile, the layer above the last of `cuts` painted before them that cuts the tile, or
// where they reach too many tiles to follow one by one, each layer above a cut painted before them, and the layer below
// all cuts unless one of those cuts every tile, in all of `reach`.
fn for_each_layer_records_go_into(
    cuts: &[BarrierCut],
    layer_below_all_cuts: LayerIndex,
    record_position: ListPosition,
    reach: IntRect,
    mut for_layer_and_part_of_reach: impl FnMut(LayerIndex, IntRect),
) {
    let count_of_cuts_painted_before = cuts
        .iter()
        .take_while(|cut| cut.barrier_first_record_position <= record_position)
        .count();
    let cuts_painted_before = &cuts[..count_of_cuts_painted_before];
    if cuts_painted_before.is_empty() || reaches_too_many_cells_to_follow_one_by_one(reach) {
        let cells_reached = CellRange::touched_by(reach);
        let a_cut_painted_before_cuts_every_cell_reached = cuts_painted_before.iter().any(|cut| match cut.cut_reach {
            Reach::Anywhere => true,
            Reach::Within(cut_rect) => CellRange::touched_by(cut_rect).holds_every_cell_of(cells_reached),
            Reach::Nothing => false,
        });
        if !a_cut_painted_before_cuts_every_cell_reached {
            for_layer_and_part_of_reach(layer_below_all_cuts, reach);
        }
        for cut in cuts_painted_before {
            for_layer_and_part_of_reach(cut.layer_above_cut, reach);
        }
        return;
    }
    for coord in tile_coords_touching(reach) {
        let cell = coord.cell_rect();
        let layer_in_cell = cuts_painted_before
            .iter()
            .rev()
            .find(|cut| cut.cuts_cell(cell))
            .map_or(layer_below_all_cuts, |cut| cut.layer_above_cut);
        for_layer_and_part_of_reach(layer_in_cell, reach.intersected(cell));
    }
}

struct StackBeingBuilt<'a> {
    placement: &'a ListPlacement,
    spaces: &'a [RasterSpace],
    nodes: Vec<NodeOfPlan>,
    node_of_run_group: Vec<PlacementNodeIndex>,
    children_of_node: Vec<Vec<PlacementNodeIndex>>,
    bands_being_built: Vec<BandBeingBuilt>,
    /// Per placed effect, once decided, the node the compositor composites it in, if it composites it.
    composited_node_once_decided_by_placed_effect: Vec<Option<Option<PlacementNodeIndex>>>,
    composited_effects_around_node_by_node_index: Vec<usize>,
    innermost_composited_node_around_by_node_index: Vec<Option<PlacementNodeIndex>>,
}

impl StackBeingBuilt<'_> {
    fn add_node(&mut self, node: NodeOfPlan) -> PlacementNodeIndex {
        let parent_index = node.parent_index;
        let composited_effects_around_parent = parent_index.map_or(0, |parent_index| {
            self.composited_effects_around_node_by_node_index[parent_index.0]
        });
        self.composited_effects_around_node_by_node_index
            .push(composited_effects_around_parent + usize::from(node.composited_effect.is_some()));
        let innermost_composited_node_around = parent_index.and_then(|parent_index| {
            if self.nodes[parent_index.0].composited_effect.is_some() {
                Some(parent_index)
            } else {
                self.innermost_composited_node_around_by_node_index[parent_index.0]
            }
        });
        self.innermost_composited_node_around_by_node_index
            .push(innermost_composited_node_around);
        self.nodes.push(node);
        self.children_of_node.push(Vec::new());
        let node_index = PlacementNodeIndex(self.nodes.len() - 1);
        if let Some(parent_index) = parent_index {
            self.children_of_node[parent_index.0].push(node_index);
        }
        node_index
    }

    // The node of the space composited within the clips inside the composited node `container`, or outside every
    // composited node where there is none, with the nodes of the spaces between them. The space of a container holds
    // the space of each node inside it.
    fn find_or_add_node_inside(
        &mut self,
        container: Option<PlacementNodeIndex>,
        raster_space_index: RasterSpaceIndex,
        composite_clips: &[CompositeClip],
    ) -> PlacementNodeIndex {
        let space_of_container = container.map(|container| self.nodes[container.0].raster_space_index);
        if let Some(container) = container
            && space_of_container == Some(raster_space_index)
            && self.nodes[container.0].composite_clips == composite_clips
        {
            return container;
        }
        if let Some(existing_node_index) = (0..self.nodes.len()).find(|&index| {
            let node = &self.nodes[index];
            node.raster_space_index == raster_space_index
                && node.composite_clips == composite_clips
                && node.composited_effect.is_none()
                && self.innermost_composited_node_around_by_node_index[index] == container
        }) {
            return PlacementNodeIndex(existing_node_index);
        }
        let spaces = self.spaces;
        let (parent_index, reach_in_parent_node) = if space_of_container == Some(raster_space_index) {
            // Composited within clips the container is not.
            (container, Reach::Anywhere)
        } else if raster_space_index == RasterSpaceIndex::VIEWPORT {
            debug_assert!(
                container.is_none(),
                "the space of a container holds the spaces of the nodes inside it"
            );
            (None, Reach::Anywhere)
        } else {
            let parent_space = spaces[raster_space_index.0].parent_index;
            let reach_in_parent_node = composite_clips
                .iter()
                .find(|clip| clip.raster_space_index == parent_space)
                .map_or(Reach::Anywhere, |clip| Reach::Within(clip.rect));
            let composite_clips_of_parent_node: Vec<CompositeClip> = composite_clips
                .iter()
                .filter(|clip| clip.raster_space_index != parent_space)
                .copied()
                .collect();
            (
                Some(self.find_or_add_node_inside(container, parent_space, &composite_clips_of_parent_node)),
                reach_in_parent_node,
            )
        };
        self.add_node(NodeOfPlan {
            raster_space_index,
            composite_clips: composite_clips.to_vec(),
            composited_effect: None,
            parent_index,
            reach_in_parent_node,
        })
    }

    fn add_composited_node_inside(
        &mut self,
        parent_index: PlacementNodeIndex,
        effect: EffectNodeIndex,
        reach_in_parent_node: Reach<IntRect>,
    ) -> PlacementNodeIndex {
        let parent_node = &self.nodes[parent_index.0];
        let composited_node = NodeOfPlan {
            raster_space_index: parent_node.raster_space_index,
            composite_clips: parent_node.composite_clips.clone(),
            composited_effect: Some(effect),
            parent_index: Some(parent_index),
            reach_in_parent_node,
        };
        self.add_node(composited_node)
    }

    // The composited node of a placed effect that holds content of several spaces, in the node of its space inside the
    // node of the one around it, if there is one. Each is added before the stack is cut into bands.
    fn node_of_effect_holding_content_of_several_spaces(
        &mut self,
        placed_effect_index: PlacedEffectIndex,
    ) -> Result<PlacementNodeIndex, WhyListIsOneLayer> {
        if let Some(Some(node_index)) = self.composited_node_once_decided_by_placed_effect[placed_effect_index.0] {
            return Ok(node_index);
        }
        let placed_effect = self.placement.placed_effects[placed_effect_index.0];
        let space = placed_effect
            .space_of_node_holding_content_of_several_spaces
            .expect("the effect holds content of several spaces");
        // Every placed effect around it holds content of several spaces too.
        let container = match placed_effect.placed_effect_around_index {
            Some(placed_effect_around_index) => {
                Some(self.node_of_effect_holding_content_of_several_spaces(placed_effect_around_index)?)
            }
            None => None,
        };
        let parent_index = self.find_or_add_node_inside(container, space, &[]);
        if self.composited_effects_around_node_by_node_index[parent_index.0] >= MAXIMUM_NESTING_OF_COMPOSITED_EFFECTS {
            return Err(WhyListIsOneLayer::CompositedEffectsNestTooDeep);
        }
        let draw_and_read_reach = self.draw_and_read_reach_of_placed_effect(placed_effect_index);
        let node_index = self.add_composited_node_inside(parent_index, placed_effect.effect, draw_and_read_reach);
        self.composited_node_once_decided_by_placed_effect[placed_effect_index.0] = Some(Some(node_index));
        Ok(node_index)
    }

    fn placed_effects_around_stretch_outermost_first(&self, stretch: &PlacedRecordStretch) -> Vec<PlacedEffectIndex> {
        let mut placed_effects_around_stretch: Vec<PlacedEffectIndex> =
            std::iter::successors(stretch.placed_effect_index, |&placed_effect_index| {
                self.placement.placed_effects[placed_effect_index.0].placed_effect_around_index
            })
            .collect();
        placed_effects_around_stretch.reverse();
        placed_effects_around_stretch
    }

    fn reach_of_stretch_with_what_effects_around_it_read(&self, stretch: &PlacedRecordStretch) -> Reach<IntRect> {
        self.placed_effects_around_stretch_outermost_first(stretch)
            .into_iter()
            .fold(Reach::Within(stretch.reach), |reach, placed_effect_index| {
                reach.united(self.placement.placed_effects[placed_effect_index.0].backdrop_read_reach)
            })
    }

    fn draw_and_read_reach_of_placed_effect(&self, placed_effect_index: PlacedEffectIndex) -> Reach<IntRect> {
        let placed_effect = &self.placement.placed_effects[placed_effect_index.0];
        Reach::Within(placed_effect.reach).united(placed_effect.backdrop_read_reach)
    }

    // A node of a space whose anchor the compositor animates reaches only where its content can show wherever the
    // animation takes it, and a node of a sticky box only where its content, with the nodes of the spaces in it, can
    // show wherever the box is held. Nodes come after the nodes they are in, so a walk from the last node sees the final
    // reach of each node before the node it is in.
    fn limit_reach_of_moving_space_nodes_to_content_swept_over_how_they_move(
        &mut self,
        swept_reach_in_parent_space_by_space_index: &[Option<IntRect>],
    ) {
        let (placement, spaces) = (self.placement, self.spaces);
        let mut content_reach_by_node_index = vec![Reach::Nothing; self.nodes.len()];
        for stretch in &placement.record_stretches {
            let node_index = self.node_of_run_group[stretch.run_group_index.0];
            content_reach_by_node_index[node_index.0] = content_reach_by_node_index[node_index.0]
                .united(self.reach_of_stretch_with_what_effects_around_it_read(stretch));
        }
        for node_index in (0..self.nodes.len()).rev() {
            let node = &mut self.nodes[node_index];
            if node.composited_effect.is_some() {
                if let Some(parent_index) = node.parent_index {
                    content_reach_by_node_index[parent_index.0] =
                        content_reach_by_node_index[parent_index.0].united(node.reach_in_parent_node);
                }
                continue;
            }
            if let Some(Some(swept_reach)) = swept_reach_in_parent_space_by_space_index.get(node.raster_space_index.0) {
                node.reach_in_parent_node = node.reach_in_parent_node.limited_to(*swept_reach);
            }
            let raster_space = &spaces[node.raster_space_index.0];
            if matches!(raster_space.anchor_kind, RasterSpaceAnchorKind::StickyBox { .. })
                && let Reach::Within(content_reach) = content_reach_by_node_index[node_index]
            {
                let content_reach_swept_over_offset_range = rect_swept_over_translations(
                    content_reach,
                    raster_space.lowest_anchor_translation,
                    raster_space.highest_anchor_translation,
                );
                node.reach_in_parent_node = node
                    .reach_in_parent_node
                    .limited_to(content_reach_swept_over_offset_range);
            }
            if let Some(parent_index) = node.parent_index {
                let reach_in_parent_node = node.reach_in_parent_node;
                content_reach_by_node_index[parent_index.0] =
                    content_reach_by_node_index[parent_index.0].united(reach_in_parent_node);
            }
        }
    }

    fn path_from_root_to(&self, mut node_index: PlacementNodeIndex) -> Vec<PlacementNodeIndex> {
        let mut path = vec![node_index];
        while let Some(parent_index) = self.nodes[node_index.0].parent_index {
            path.push(parent_index);
            node_index = parent_index;
        }
        path.reverse();
        path
    }

    fn open_band(
        &mut self,
        placement_node_index: PlacementNodeIndex,
        first_record_stretch_index: RecordStretchIndex,
    ) -> BandBeingBuiltIndex {
        self.bands_being_built.push(BandBeingBuilt {
            placement_node_index,
            barriers: Vec::new(),
            own_record_stretch_indices: Vec::new(),
            first_record_stretch_index,
            root_group_reach_so_far: Reach::Nothing,
        });
        BandBeingBuiltIndex(self.bands_being_built.len() - 1)
    }

    fn reach_can_meet_tiles(
        &self,
        node_of_reach: PlacementNodeIndex,
        reach_in_node: Reach<IntRect>,
        path_from_root_to_node_of_tiles: &[PlacementNodeIndex],
        tiles_in_node: IntRect,
    ) -> bool {
        if reach_in_node == Reach::Nothing {
            return false;
        }
        let path_from_root_to_node_of_reach = self.path_from_root_to(node_of_reach);
        let length_of_shared_path = path_from_root_to_node_of_reach
            .iter()
            .zip(path_from_root_to_node_of_tiles)
            .take_while(|(first, second)| first == second)
            .count();
        let reach_in_lowest_shared_node = match path_from_root_to_node_of_reach.get(length_of_shared_path) {
            Some(&child_of_lowest_shared_node) => self.nodes[child_of_lowest_shared_node.0].reach_in_parent_node,
            None => reach_in_node,
        };
        let tiles_in_lowest_shared_node = match path_from_root_to_node_of_tiles.get(length_of_shared_path) {
            Some(&child_of_lowest_shared_node) => self.nodes[child_of_lowest_shared_node.0].reach_in_parent_node,
            None => Reach::Within(tiles_in_node),
        };
        reach_in_lowest_shared_node.meets(tiles_in_lowest_shared_node)
    }

    fn reader_painted_next_finds_all_it_reads_in_own_layer(
        &self,
        reader_node: PlacementNodeIndex,
        open_band_of_reader_node: Option<BandBeingBuiltIndex>,
        read_tiles: IntRect,
    ) -> bool {
        let path_from_root_to_reader_node = self.path_from_root_to(reader_node);
        let another_band_shows_root_group_in_read_tiles =
            self.bands_being_built.iter().enumerate().any(|(index, band)| {
                Some(BandBeingBuiltIndex(index)) != open_band_of_reader_node
                    && self.reach_can_meet_tiles(
                        band.placement_node_index,
                        band.root_group_reach_so_far,
                        &path_from_root_to_reader_node,
                        read_tiles,
                    )
            });
        if another_band_shows_root_group_in_read_tiles {
            return false;
        }
        let Some(open_band_of_reader_node) = open_band_of_reader_node else {
            return true;
        };
        self.bands_being_built[open_band_of_reader_node.0]
            .barriers
            .iter()
            .all(|barrier| {
                let reach_of_barrier = match *barrier {
                    BarrierBeingBuilt::Media { record_stretch_index } => {
                        Reach::Within(self.placement.record_stretches[record_stretch_index.0].reach)
                    }
                    BarrierBeingBuilt::NestedNodeBand { band_being_built_index } => {
                        let nested_node_index = self.bands_being_built[band_being_built_index.0].placement_node_index;
                        self.nodes[nested_node_index.0].reach_in_parent_node
                    }
                };
                !reach_of_barrier.touches(read_tiles)
            })
    }

    fn close_bands_of_node_and_nodes_inside(
        &self,
        node_index: PlacementNodeIndex,
        open_band_by_node_index: &mut [Option<BandBeingBuiltIndex>],
    ) {
        let mut nodes_to_close = vec![node_index];
        while let Some(node_index) = nodes_to_close.pop() {
            open_band_by_node_index[node_index.0] = None;
            nodes_to_close.extend_from_slice(&self.children_of_node[node_index.0]);
        }
    }

    fn placed_effect_is_composited(
        &self,
        placed_effect: &PlacedEffect,
        node_of_first_stretch_painted_next: PlacementNodeIndex,
        open_band_of_node: Option<BandBeingBuiltIndex>,
    ) -> bool {
        if placed_effect.why_composited_regardless_of_layer_content.is_some() {
            return true;
        }
        let finds_all_it_reads_in_own_layer = match placed_effect.backdrop_read_reach {
            Reach::Nothing => true,
            Reach::Within(backdrop_read_rect) => self.reader_painted_next_finds_all_it_reads_in_own_layer(
                node_of_first_stretch_painted_next,
                open_band_of_node,
                CellRange::touched_by(backdrop_read_rect).rect(),
            ),
            Reach::Anywhere => false,
        };
        !finds_all_it_reads_in_own_layer
    }

    fn node_of_stretch(
        &mut self,
        stretch: &PlacedRecordStretch,
        open_band_by_node_index: &mut Vec<Option<BandBeingBuiltIndex>>,
    ) -> Result<PlacementNodeIndex, WhyListIsOneLayer> {
        let mut node_index = self.node_of_run_group[stretch.run_group_index.0];
        for placed_effect_index in self.placed_effects_around_stretch_outermost_first(stretch) {
            if self.placement.placed_effects[placed_effect_index.0]
                .space_of_node_holding_content_of_several_spaces
                .is_some()
            {
                continue;
            }
            let composited_node_index = match self.composited_node_once_decided_by_placed_effect[placed_effect_index.0]
            {
                Some(composited_node_index) => composited_node_index,
                None => {
                    let placed_effect = self.placement.placed_effects[placed_effect_index.0];
                    let composited_node_index = if self.placed_effect_is_composited(
                        &placed_effect,
                        node_index,
                        open_band_by_node_index[node_index.0],
                    ) {
                        if self.composited_effects_around_node_by_node_index[node_index.0]
                            >= MAXIMUM_NESTING_OF_COMPOSITED_EFFECTS
                        {
                            return Err(WhyListIsOneLayer::CompositedEffectsNestTooDeep);
                        }
                        // Media and animations get no layer inside an effect the compositor cannot composite, but an
                        // effect that reads what is below its layer can still make one composited.
                        if let Some(why) = self.placement.why_compositor_cannot_composite_by_effect_index
                            [placed_effect.effect.0 as usize]
                        {
                            return Err(WhyListIsOneLayer::EffectTheCompositorCannotCompositeIsComposited(why));
                        }
                        let draw_and_read_reach = self.draw_and_read_reach_of_placed_effect(placed_effect_index);
                        let composited_node_index =
                            self.add_composited_node_inside(node_index, placed_effect.effect, draw_and_read_reach);
                        open_band_by_node_index.push(None);
                        Some(composited_node_index)
                    } else {
                        None
                    };
                    self.composited_node_once_decided_by_placed_effect[placed_effect_index.0] =
                        Some(composited_node_index);
                    composited_node_index
                }
            };
            let Some(composited_node_index) = composited_node_index else {
                break;
            };
            node_index = composited_node_index;
        }
        Ok(node_index)
    }

    fn cut_into_bands(&mut self) -> Result<(), WhyListIsOneLayer> {
        let placement = self.placement;
        let root_node_index = PlacementNodeIndex(0);
        let mut open_band_by_node_index: Vec<Option<BandBeingBuiltIndex>> = vec![None; self.nodes.len()];
        open_band_by_node_index[root_node_index.0] = Some(self.open_band(root_node_index, RecordStretchIndex(0)));
        let mut composited_nodes_whose_bands_may_be_open: Vec<PlacementNodeIndex> = Vec::new();
        let mut node_is_on_path_of_stretch: Vec<bool> = Vec::new();
        for (index, stretch) in placement.record_stretches.iter().enumerate() {
            let stretch_index = RecordStretchIndex(index);
            if stretch.reach.is_empty() {
                continue;
            }
            let node_of_stretch = self.node_of_stretch(stretch, &mut open_band_by_node_index)?;
            let reach_read_by_blending_records =
                placement.run_groups[stretch.run_group_index.0].reach_read_by_records_blending_with_root_group;
            if reach_read_by_blending_records != Reach::Nothing {
                let blending_records_find_all_they_read_in_own_layer = match reach_read_by_blending_records {
                    Reach::Within(read_rect) => {
                        let tiles_blending_records_draw_or_read =
                            CellRange::touched_by(stretch.reach.united(read_rect)).rect();
                        self.reader_painted_next_finds_all_it_reads_in_own_layer(
                            node_of_stretch,
                            open_band_by_node_index[node_of_stretch.0],
                            tiles_blending_records_draw_or_read,
                        )
                    }
                    _ => false,
                };
                if !blending_records_find_all_they_read_in_own_layer {
                    return Err(WhyListIsOneLayer::RecordsBlendWithWhatOtherLayersHoldBelowThem);
                }
            }
            let path_from_root_to_node_of_stretch = self.path_from_root_to(node_of_stretch);
            // A replay of the list composites an effect anew wherever content outside it is painted, so the bands of
            // its node end there.
            composited_nodes_whose_bands_may_be_open.retain(|node| open_band_by_node_index[node.0].is_some());
            if !composited_nodes_whose_bands_may_be_open.is_empty() {
                node_is_on_path_of_stretch.resize(self.nodes.len(), false);
                for node_on_path in &path_from_root_to_node_of_stretch {
                    node_is_on_path_of_stretch[node_on_path.0] = true;
                }
                for &composited_node_index in &composited_nodes_whose_bands_may_be_open {
                    if open_band_by_node_index[composited_node_index.0].is_some()
                        && !node_is_on_path_of_stretch[composited_node_index.0]
                    {
                        self.close_bands_of_node_and_nodes_inside(composited_node_index, &mut open_band_by_node_index);
                    }
                }
                for node_on_path in &path_from_root_to_node_of_stretch {
                    node_is_on_path_of_stretch[node_on_path.0] = false;
                }
            }
            for depth in 0..path_from_root_to_node_of_stretch.len() {
                let node_on_path = path_from_root_to_node_of_stretch[depth];
                let open_band_of_node_on_path =
                    open_band_by_node_index[node_on_path.0].expect("the bands along the path of a stretch are open");
                let next_node_on_path = path_from_root_to_node_of_stretch.get(depth + 1).copied();
                let reach_of_stretch_in_node_on_path = match next_node_on_path {
                    Some(next_node) => self.nodes[next_node.0].reach_in_parent_node,
                    None => Reach::Within(stretch.reach),
                };
                for &other_child in &self.children_of_node[node_on_path.0] {
                    if Some(other_child) == next_node_on_path || open_band_by_node_index[other_child.0].is_none() {
                        continue;
                    }
                    if reach_of_stretch_in_node_on_path.meets(self.nodes[other_child.0].reach_in_parent_node) {
                        self.close_bands_of_node_and_nodes_inside(other_child, &mut open_band_by_node_index);
                    }
                }
                match next_node_on_path {
                    Some(next_node) if open_band_by_node_index[next_node.0].is_none() => {
                        let new_band_of_next_node = self.open_band(next_node, stretch_index);
                        open_band_by_node_index[next_node.0] = Some(new_band_of_next_node);
                        if self.nodes[next_node.0].composited_effect.is_some() {
                            composited_nodes_whose_bands_may_be_open.push(next_node);
                        }
                        self.bands_being_built[open_band_of_node_on_path.0].barriers.push(
                            BarrierBeingBuilt::NestedNodeBand {
                                band_being_built_index: new_band_of_next_node,
                            },
                        );
                    }
                    Some(_) => {}
                    None if stretch.placed_media_index.is_some() => {
                        self.bands_being_built[open_band_of_node_on_path.0]
                            .barriers
                            .push(BarrierBeingBuilt::Media {
                                record_stretch_index: stretch_index,
                            });
                    }
                    None => self.bands_being_built[open_band_of_node_on_path.0]
                        .own_record_stretch_indices
                        .push(stretch_index),
                }
            }
            if placement.run_groups[stretch.run_group_index.0].paints_within_root_group {
                let open_band_of_stretch = open_band_by_node_index[node_of_stretch.0]
                    .expect("the band of a stretch is open once it is placed");
                let band_of_stretch = &mut self.bands_being_built[open_band_of_stretch.0];
                band_of_stretch.root_group_reach_so_far = band_of_stretch
                    .root_group_reach_so_far
                    .united(Reach::Within(stretch.reach));
            }
        }
        Ok(())
    }

    // Puts the band into `bands`, and after it the bands nested in it, in the order of a walk that enters a nested band
    // at its barrier.
    fn emit_band(&mut self, band_being_built_index: BandBeingBuiltIndex, bands: &mut Vec<BandOfPlan>) {
        let placement = self.placement;
        let band_being_built = &mut self.bands_being_built[band_being_built_index.0];
        let placement_node_index = band_being_built.placement_node_index;
        let own_record_stretch_indices = std::mem::take(&mut band_being_built.own_record_stretch_indices);
        let barriers = std::mem::take(&mut band_being_built.barriers);
        let band_position = bands.len();
        bands.push(BandOfPlan {
            placement_node_index,
            record_spans: Vec::new(),
            reach_of_layer_below_all_cuts: IntRect::default(),
            cuts: Vec::new(),
        });
        // Here a layer index counts the band's painted layers: the one below all cuts, then the one above each cut.
        let mut cuts = Vec::with_capacity(barriers.len());
        let mut barriers_of_plan = Vec::with_capacity(barriers.len());
        for barrier in barriers {
            let (barrier_of_plan, barrier_first_record_position, cut_reach) = match barrier {
                BarrierBeingBuilt::Media { record_stretch_index } => {
                    let media_stretch = &placement.record_stretches[record_stretch_index.0];
                    let media = &placement.placed_media[media_stretch
                        .placed_media_index
                        .expect("a media barrier is a media stretch")
                        .0];
                    let barrier_of_plan = BarrierOfPlan::Media {
                        content: media.content,
                        ordinal: media.ordinal,
                        record_span: media_stretch.record_span,
                        reach: media_stretch.reach,
                        cuts_band: media.cuts_band,
                    };
                    let cut_reach = if media.cuts_band {
                        Reach::Within(media_stretch.reach)
                    } else {
                        Reach::Nothing
                    };
                    (barrier_of_plan, media_stretch.record_span.begin, cut_reach)
                }
                BarrierBeingBuilt::NestedNodeBand {
                    band_being_built_index: nested_band_being_built_index,
                } => {
                    let nested_band = &self.bands_being_built[nested_band_being_built_index.0];
                    let first_record_position_of_nested_band = placement.record_stretches
                        [nested_band.first_record_stretch_index.0]
                        .record_span
                        .begin;
                    let cut_reach = self.nodes[nested_band.placement_node_index.0].reach_in_parent_node;
                    self.emit_band(nested_band_being_built_index, bands);
                    (
                        BarrierOfPlan::NestedBand,
                        first_record_position_of_nested_band,
                        cut_reach,
                    )
                }
            };
            cuts.push(BarrierCut {
                barrier_first_record_position,
                cut_reach,
                layer_above_cut: LayerIndex(cuts.len() + 1),
            });
            barriers_of_plan.push(barrier_of_plan);
        }

        // A record draws nothing past the reach of its stretch, so the layers it is replayed into anywhere else stay
        // empty.
        let mut reach_of_painted_layers = vec![IntRect::default(); cuts.len() + 1];
        let mut record_spans: Vec<ListSpan> = Vec::with_capacity(own_record_stretch_indices.len());
        for &stretch_index in &own_record_stretch_indices {
            let stretch = &placement.record_stretches[stretch_index.0];
            let stretch_span = stretch.record_span;
            for_each_layer_records_go_into(
                &cuts,
                LayerIndex(0),
                stretch_span.begin,
                stretch.reach,
                |layer, part_of_reach| {
                    reach_of_painted_layers[layer.0] = reach_of_painted_layers[layer.0].united(part_of_reach);
                },
            );
            match record_spans.last_mut() {
                Some(last) if last.end == stretch_span.begin => last.end = stretch_span.end,
                _ => record_spans.push(stretch_span),
            }
        }
        let band = &mut bands[band_position];
        band.record_spans = record_spans;
        band.reach_of_layer_below_all_cuts = reach_of_painted_layers[0];
        band.cuts = barriers_of_plan
            .into_iter()
            .zip(&reach_of_painted_layers[1..])
            .map(|(barrier, &reach_of_layer_above_cut)| CutOfPlan {
                barrier,
                reach_of_layer_above_cut,
            })
            .collect();
    }
}

// The plan of the stack, or the first reason found while the stack is cut into bands why the list goes as one layer
// after all.
fn assemble_plan(
    inputs: &LayerStackInputs<'_>,
    placement: &ListPlacement,
    swept_reach_in_parent_space_by_space_index: Vec<Option<IntRect>>,
    tiles_hold_content_moving_within_space_by_space_index: Vec<bool>,
    spaces: &[RasterSpace],
    layering_obstacles: &LayeringObstacles,
) -> Result<LayeredPlan, WhyListIsOneLayer> {
    let mut stack_being_built = StackBeingBuilt {
        placement,
        spaces,
        nodes: Vec::new(),
        node_of_run_group: Vec::new(),
        children_of_node: Vec::new(),
        bands_being_built: Vec::new(),
        composited_node_once_decided_by_placed_effect: vec![None; placement.placed_effects.len()],
        composited_effects_around_node_by_node_index: Vec::new(),
        innermost_composited_node_around_by_node_index: Vec::new(),
    };
    stack_being_built.find_or_add_node_inside(None, RasterSpaceIndex::VIEWPORT, &[]);
    let placed_effect_index_by_effect_holding_content_of_several_spaces: HashMap<EffectNodeIndex, PlacedEffectIndex> =
        placement
            .placed_effects
            .iter()
            .enumerate()
            .filter(|(_, placed_effect)| placed_effect.space_of_node_holding_content_of_several_spaces.is_some())
            .map(|(index, placed_effect)| (placed_effect.effect, PlacedEffectIndex(index)))
            .collect();
    let mut node_of_run_group = Vec::with_capacity(placement.run_groups.len());
    for run_group in &placement.run_groups {
        let container = match run_group.inside_effect_holding_content_of_several_spaces {
            Some(effect) => Some(stack_being_built.node_of_effect_holding_content_of_several_spaces(
                placed_effect_index_by_effect_holding_content_of_several_spaces[&effect],
            )?),
            None => None,
        };
        node_of_run_group.push(stack_being_built.find_or_add_node_inside(
            container,
            run_group.raster_space_index,
            &run_group.composite_clips,
        ));
    }
    stack_being_built.node_of_run_group = node_of_run_group;
    stack_being_built.limit_reach_of_moving_space_nodes_to_content_swept_over_how_they_move(
        &swept_reach_in_parent_space_by_space_index,
    );
    stack_being_built.cut_into_bands()?;
    if layering_obstacles.sorts_planes_by_depth
        && contexts_sorting_planes_by_depth_hold_effects_that_read_or_are_composited(
            inputs,
            placement,
            &stack_being_built.nodes,
        )
    {
        return Err(WhyListIsOneLayer::SortingContext3D);
    }
    let mut bands = Vec::with_capacity(stack_being_built.bands_being_built.len());
    stack_being_built.emit_band(BandBeingBuiltIndex(0), &mut bands);

    let plan = LayeredPlan {
        nodes: stack_being_built.nodes,
        bands,
        tiles_hold_content_moving_within_space_by_space_index,
        swept_reach_in_parent_space_by_space_index,
    };
    if plan_holds_more_than_compositor_decodes(&plan) {
        return Err(WhyListIsOneLayer::PlanHoldsMoreThanCompositorDecodes);
    }
    Ok(plan)
}

// A 3D sorting context that sorts its planes by depth is placed whole into the tiles of one band, which replay it
// whole, as sorting its planes reads all of them. That holds only where nothing inside it reads what is below it or is
// composited apart from it.
fn contexts_sorting_planes_by_depth_hold_effects_that_read_or_are_composited(
    inputs: &LayerStackInputs<'_>,
    placement: &ListPlacement,
    nodes: &[NodeOfPlan],
) -> bool {
    let tree = inputs.tree;
    let run_groups = inputs.list.run_groups(tree);
    let mut marks = [effects_of_marked_runs(inputs.list, tree, |run_index| {
        run_groups.run_is_sorted_by_depth(run_index)
    })];
    mark_effects_around_marked_effects(tree, &mut marks);
    let [effect_holds_run_sorted_by_depth_by_effect_index] = marks;
    placement
        .backdrop_readers_in_paint_order
        .iter()
        .copied()
        .chain(nodes.iter().filter_map(|node| node.composited_effect))
        .any(|effect| effect_holds_run_sorted_by_depth_by_effect_index[effect.0 as usize])
}

/// Works out the layers of a list from where `place_list` placed its parts among `spaces`, whose extents are not filled
/// in.
///
/// A plan of more nodes, bands, cuts or spans than the compositor decodes, or of composited effects nested deeper than
/// `MAXIMUM_NESTING_OF_COMPOSITED_EFFECTS`, gives the list one layer. Within those limits no layer that an effect the
/// compositor composites or a canvas, video or nested context needs is given up, as that would draw the list wrong,
/// and a layer holds tiles only where its content reaches, which the pool of tiles gives back.
///
/// A plan of more layers that fails the checks the compositor runs on a plan it receives is a bug of the planner. A
/// build with debug assertions panics on it, and other builds give the list one layer.
fn build_layer_stack(
    inputs: &LayerStackInputs<'_>,
    placement: ListPlacement,
    swept_reach_in_parent_space_by_space_index: Vec<Option<IntRect>>,
    tiles_hold_content_moving_within_space_by_space_index: Vec<bool>,
    spaces: Vec<RasterSpace>,
    layering_obstacles: LayeringObstacles,
    nesting_of_live_effects: &NestingOfLiveEffects<'_>,
) -> PlannedLayers {
    let stack_of_plan = assemble_plan(
        inputs,
        &placement,
        swept_reach_in_parent_space_by_space_index,
        tiles_hold_content_moving_within_space_by_space_index,
        &spaces,
        &layering_obstacles,
    )
    .and_then(|plan| {
        let stack_on_wire = StackOnWire::Layered(plan.clone());
        LayerStack::of_plan_passing_checks(
            plan,
            spaces,
            inputs.list,
            inputs.tree,
            inputs.viewport_rect,
            layering_obstacles,
            nesting_of_live_effects,
        )
        .map(|stack| (stack, stack_on_wire))
        .map_err(|why_plan_fails_checks| {
            debug_assert!(
                false,
                "the compositor refuses a plan of layers the planner made: {why_plan_fails_checks}"
            );
            eprintln!(
                "WebContent: Sending one layer, as the compositor refuses the plan of layers: {why_plan_fails_checks}"
            );
            WhyListIsOneLayer::PlanFailsChecksOfCompositor
        })
    });
    match stack_of_plan {
        Ok((stack, stack_on_wire)) => PlannedLayers {
            stack,
            stack_on_wire,
            placement,
        },
        Err(why_list_is_one_layer) => one_layer(inputs, why_list_is_one_layer, layering_obstacles, placement),
    }
}

fn one_layer(
    inputs: &LayerStackInputs<'_>,
    why_list_is_one_layer: WhyListIsOneLayer,
    layering_obstacles: LayeringObstacles,
    placement: ListPlacement,
) -> PlannedLayers {
    PlannedLayers {
        stack: LayerStack::one_layer_replaying_whole_list(
            inputs.viewport_rect,
            inputs.list.whole_span(),
            Some(why_list_is_one_layer),
            layering_obstacles,
        ),
        stack_on_wire: StackOnWire::OneLayer(why_list_is_one_layer),
        placement,
    }
}

// The scroll offsets a plan reads of `scroll_offsets`: zero for each node that `leaves_out_scroll_offset_of_node` names.
fn scroll_offsets_layer_plans_read(
    scroll_offsets: &[FloatPoint],
    leaves_out_scroll_offset_of_node: &[bool],
) -> Vec<FloatPoint> {
    scroll_offsets
        .iter()
        .enumerate()
        .map(|(node_index, &scroll_offset)| {
            if leaves_out_scroll_offset_of_node
                .get(node_index)
                .copied()
                .unwrap_or(false)
            {
                FloatPoint::default()
            } else {
                scroll_offset
            }
        })
        .collect()
}

/// Plans the layers of a list, and gives the stack with where it placed the parts of the list. The plan holds for every
/// sample of the compositor's animations and wherever the compositor scrolls the nodes it scrolls. Where the window
/// server can show the media of the stack is left to be worked out for each frame.
pub fn plan_layer_stack(inputs: &LayerStackInputs<'_>) -> PlannedLayers {
    let anchors_of_tree = RasterSpaceAnchorsOfTree::of(inputs.tree);
    let scroll_offsets_read =
        scroll_offsets_layer_plans_read(inputs.scroll_offsets, &anchors_of_tree.leaves_out_scroll_offset_of_node);
    plan_layer_stack_at_anchors(
        &LayerStackInputs {
            scroll_offsets: &scroll_offsets_read,
            ..*inputs
        },
        &anchors_of_tree,
    )
}

// `anchors_of_tree` are those of `inputs.tree`, and `inputs.scroll_offsets` hold zero for each node whose offset
// `RasterSpaceAnchorsOfTree::leaves_out_scroll_offset_of_node` names.
fn plan_layer_stack_at_anchors(
    inputs: &LayerStackInputs<'_>,
    anchors_of_tree: &RasterSpaceAnchorsOfTree,
) -> PlannedLayers {
    let nesting_of_live_effects = NestingOfLiveEffects::of(inputs.tree);
    let layering_obstacles = LayeringObstacles::of(inputs.tree);
    if let Some(why) = why_list_cannot_be_layered(inputs) {
        return one_layer(inputs, why, layering_obstacles, ListPlacement::default());
    }
    let plan_giving_layers_to_at_most = |most_canvas_video_and_nested_context_layers: Option<usize>| {
        let (spaces, geometries) = spaces_with_geometries(
            inputs.tree,
            anchors_of_tree,
            inputs.scroll_offsets,
            inputs.viewport_rect,
        );
        let spaces_of_stack = spaces.spaces().to_vec();
        let (
            placement,
            swept_reach_in_parent_space_by_space_index,
            tiles_hold_content_moving_within_space_by_space_index,
        ) = place_list(
            inputs,
            spaces,
            &geometries,
            &nesting_of_live_effects,
            most_canvas_video_and_nested_context_layers,
        );
        build_layer_stack(
            inputs,
            placement,
            swept_reach_in_parent_space_by_space_index,
            tiles_hold_content_moving_within_space_by_space_index,
            spaces_of_stack,
            layering_obstacles,
            &nesting_of_live_effects,
        )
    };
    let planned_with_layers_for_all = plan_giving_layers_to_at_most(None);
    let canvases_videos_and_nested_contexts_cut_bands_more_than_compositor_decodes =
        planned_with_layers_for_all.stack.why_list_is_one_layer
            == Some(WhyListIsOneLayer::PlanHoldsMoreThanCompositorDecodes)
            && planned_with_layers_for_all
                .placement
                .placed_media
                .iter()
                .filter(|media| !media.content.is_drawn_by_its_record())
                .count()
                > CANVAS_VIDEO_AND_NESTED_CONTEXT_LAYERS_A_BAND_LEAVES_CUTS_FOR;
    if canvases_videos_and_nested_contexts_cut_bands_more_than_compositor_decodes {
        return plan_giving_layers_to_at_most(Some(CANVAS_VIDEO_AND_NESTED_CONTEXT_LAYERS_A_BAND_LEAVES_CUTS_FOR));
    }
    planned_with_layers_for_all
}

/// What a plan of the layers of a list is made from, all of which WebContent publishes: the list, the tree, the viewport
/// rect, and of the scroll offsets, those the plan reads. A plan made from inputs holds for others that plan alike.
pub struct InputsOfLayerPlan {
    list: Arc<DisplayListStorage>,
    tree: Arc<VisualContextTree>,
    viewport_rect: IntRect,
    anchors_of_tree: Arc<RasterSpaceAnchorsOfTree>,
    scroll_offsets_plan_reads: Vec<FloatPoint>,
}

impl InputsOfLayerPlan {
    pub fn new(
        list: Arc<DisplayListStorage>,
        tree: Arc<VisualContextTree>,
        scroll_offsets: &[FloatPoint],
        viewport_rect: IntRect,
    ) -> Self {
        Self::new_after(None, list, tree, scroll_offsets, viewport_rect)
    }

    /// As `new`, but where `earlier` inputs were made for the same tree, what plans work out of the tree alone comes
    /// from them.
    pub fn new_after(
        earlier: Option<&Self>,
        list: Arc<DisplayListStorage>,
        tree: Arc<VisualContextTree>,
        scroll_offsets: &[FloatPoint],
        viewport_rect: IntRect,
    ) -> Self {
        let tree = VisualContextTree::tree_of_layer_plans_for(&tree, earlier.map(|earlier| &earlier.tree));
        let anchors_of_tree = match earlier {
            Some(earlier) if Arc::ptr_eq(&earlier.tree, &tree) => earlier.anchors_of_tree.clone(),
            _ => Arc::new(RasterSpaceAnchorsOfTree::of(&tree)),
        };
        Self {
            scroll_offsets_plan_reads: scroll_offsets_layer_plans_read(
                scroll_offsets,
                &anchors_of_tree.leaves_out_scroll_offset_of_node,
            ),
            list,
            tree,
            viewport_rect,
            anchors_of_tree,
        }
    }

    /// The inputs of the same list and tree under other scroll offsets and another viewport rect.
    pub fn with_scroll_offsets_and_viewport_rect(&self, scroll_offsets: &[FloatPoint], viewport_rect: IntRect) -> Self {
        Self::new_after(
            Some(self),
            self.list.clone(),
            self.tree.clone(),
            scroll_offsets,
            viewport_rect,
        )
    }

    pub fn viewport_rect(&self) -> IntRect {
        self.viewport_rect
    }

    /// Whether a plan made from these inputs is the plan of `other`: the lists hold the same chunks in the same order,
    /// the trees have the same content, the viewport rects are the same, and the plan reads the same scroll offsets.
    pub fn plan_alike_as(&self, other: &Self) -> bool {
        // A list of scroll offsets leaves out the zero offsets of the nodes past its end.
        let offset_at =
            |offsets: &[FloatPoint], node_index: usize| offsets.get(node_index).copied().unwrap_or_default();
        let node_count = self
            .scroll_offsets_plan_reads
            .len()
            .max(other.scroll_offsets_plan_reads.len());
        self.viewport_rect == other.viewport_rect
            && (0..node_count).all(|node_index| {
                offset_at(&self.scroll_offsets_plan_reads, node_index)
                    == offset_at(&other.scroll_offsets_plan_reads, node_index)
            })
            && (Arc::ptr_eq(&self.list, &other.list) || self.list.holds_same_chunks_in_same_order_as(&other.list))
            && (Arc::ptr_eq(&self.tree, &other.tree) || self.tree.has_same_content_as(&other.tree))
    }

    /// Plans the layers of the list under these inputs.
    pub fn plan(&self) -> PlannedLayers {
        plan_layer_stack_at_anchors(
            &LayerStackInputs {
                list: &self.list,
                tree: &self.tree,
                scroll_offsets: &self.scroll_offsets_plan_reads,
                viewport_rect: self.viewport_rect,
            },
            &self.anchors_of_tree,
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::display_list::commands::{CanvasId, CompositorContextId, ContextRef, FillRoundedRectRing};
    use crate::display_list::layers::{
        BarrierCut, BarrierIdentity, CutsOfCell, LayerIdentity, NodeNamesFromIndices, WindowServerPresentationOfMedia,
        media_name,
    };
    use crate::display_list::test_fixtures::{
        GREEN, ListOfOneChunk, VIEWPORT, animation_of, canvas, caret, effects, fill, frosted_glass, nested_context,
        opacity_animation_of, scroll_bar, scroll_node, scroll_record, spinning, transform, transform_animation_of,
        tree_with_document_scroll, tree_with_document_scroller, tree_with_root_isolation_effect,
    };
    use crate::display_list::tiles::TILE_HEIGHT_IN_DEVICE_PIXELS;
    use crate::node_slot_id::NodeSlotId;
    use crate::visual_context::{
        CompositorScrollNodeRecord, CompositorScrollTable, EffectsData, SpatialData, TransformData, TransformDataRole,
    };
    use libgfx_rust::{Color, CompositingAndBlendingOperator, FloatMatrix4x4, FloatRect};

    fn kinds_of_layers_from_base_up_that_may_hold_content(stack: &LayerStack) -> Vec<String> {
        stack
            .layers
            .iter()
            .enumerate()
            .filter(|(index, layer)| {
                *index == stack.base_layer.0 || (*index > stack.base_layer.0 && layer.may_hold_content)
            })
            .map(|(index, layer)| {
                let space = stack.name_of_space(
                    stack.nodes[layer.placement_node_index.0].raster_space_index,
                    &NodeNamesFromIndices,
                );
                match layer.identity {
                    LayerIdentity::Painted {
                        band_ordinal_in_node: band,
                        above_barrier,
                        ..
                    } => {
                        let kind = if index == stack.base_layer.0 { "base" } else { "painted" };
                        let above = above_barrier.map_or(String::new(), |_| {
                            format!(
                                " above {}",
                                stack.name_of_barrier_below(LayerIndex(index), &NodeNamesFromIndices)
                            )
                        });
                        format!("{kind} {space} band {band}{above}")
                    }
                    LayerIdentity::Media { content, ordinal } => format!("{} #{ordinal} {space}", media_name(content)),
                }
            })
            .collect()
    }

    #[test]
    fn samples_of_a_transform_animation_leave_the_placement_alike() {
        let (mut tree, _, document) = tree_with_document_scroll();
        let moving = transformed_context(&mut tree, document, FloatRect::new(0.0, 0.0, 50.0, 50.0));
        tree.set_visual_animations(vec![transform_animation_of(
            moving.spatial,
            crate::host::FfiVisualAnimationTransformOperationKind::TranslateX,
            0.0,
            400.0,
        )]);
        let list = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(canvas(IntRect::new(500, 300, 200, 100), 1), document)
            .with(fill(IntRect::new(100, 300, 50, 50), GREEN), moving);
        let at_start = list.planned(&tree.with_visual_animation_samples(0), &[]);
        let halfway = list.planned(&tree.with_visual_animation_samples(500_000_000), &[]);
        assert_eq!(at_start.placement, halfway.placement);
        assert_eq!(
            at_start.stack.swept_reach_in_parent_space_by_space_index,
            halfway.stack.swept_reach_in_parent_space_by_space_index
        );
        assert_eq!(
            at_start.stack.tiles_hold_content_moving_within_space_by_space_index,
            halfway.stack.tiles_hold_content_moving_within_space_by_space_index
        );
        // Wherever the animation takes the box, it can go over the canvas, so it sits above it.
        assert_eq!(
            kinds_of_layers_from_base_up_that_may_hold_content(&halfway.stack),
            [
                "base document band 0",
                "canvas #0 document",
                "painted document band 0 above L2"
            ]
        );
    }

    #[test]
    fn a_fixed_header_painted_after_a_scrolling_document_leaves_the_base_layer_in_the_document() {
        let (tree, viewport, document) = tree_with_document_scroll();
        let list = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(fill(IntRect::new(0, 0, 800, 50), GREEN), viewport);
        let planned = list.planned(&tree, &[]);
        assert_eq!(
            kinds_of_layers_from_base_up_that_may_hold_content(&planned.stack),
            [
                "base document band 0",
                "painted viewport band 0 above band 0 of document"
            ]
        );
        let storage = DisplayListStorage::of_builder(&list.builder);
        assert_eq!(planned.layers_of_record(storage.span_of_run(1).begin).len(), 1);
    }

    #[test]
    fn content_reaching_too_many_cells_to_follow_marks_no_layer_below_a_cut_that_covers_it() {
        let (mut tree, _, document) = tree_with_document_scroll();
        let taller_than_the_cells_followed_one_by_one = IntRect::new(0, 0, 800, 4100 * TILE_HEIGHT_IN_DEVICE_PIXELS);
        let mut scroll_table = tree.compositor_scroll_table().clone();
        let mut document_scrolling_past_the_cells_followed_one_by_one = scroll_table.nodes.to_vec();
        document_scrolling_past_the_cells_followed_one_by_one[0]
            .max_scroll_offset
            .y = taller_than_the_cells_followed_one_by_one.height as f32;
        scroll_table.nodes = document_scrolling_past_the_cells_followed_one_by_one.into();
        assert!(tree.set_compositor_scroll_table(scroll_table));
        let sliding_panel = transformed_context(
            &mut tree,
            document,
            taller_than_the_cells_followed_one_by_one.to_float(),
        );
        tree.set_visual_animations(vec![sliding(sliding_panel.spatial)]);
        let list = ListOfOneChunk::default()
            .with(fill(taller_than_the_cells_followed_one_by_one, GREEN), sliding_panel)
            .with(fill(taller_than_the_cells_followed_one_by_one, GREEN), document);
        let planned = list.planned(&tree, &[]);
        assert_eq!(
            kinds_of_layers_from_base_up_that_may_hold_content(&planned.stack),
            [
                "base viewport band 0",
                "painted animated@s2 in document band 0",
                "painted document band 0 above band 0 of animated@s2 in document"
            ]
        );
    }

    fn sliding(node: SpatialNodeIndex) -> crate::visual_animation::VisualAnimation {
        transform_animation_of(
            node,
            crate::host::FfiVisualAnimationTransformOperationKind::TranslateX,
            0.0,
            300.0,
        )
    }

    fn transformed_context(tree: &mut VisualContextTree, parent: ContextRef, bounds: FloatRect) -> ContextRef {
        let spatial = tree.append_spatial(SpatialData::Transform(transform(bounds)), parent.spatial);
        ContextRef { spatial, ..parent }
    }

    fn clipped_context(
        tree: &mut VisualContextTree,
        parent: ContextRef,
        rect: IntRect,
        corner_radii: libgfx_rust::CornerRadii,
    ) -> ContextRef {
        let clip = tree.append_clip(
            crate::visual_context::ClipNodeData::Rect(crate::visual_context::ClipData {
                rect: rect.to_float(),
                corner_radii,
                mode: crate::display_list::commands::ClipMode::Intersect,
            }),
            parent.clip,
            parent.spatial,
        );
        ContextRef { clip, ..parent }
    }

    // LibWeb appends an element's effects node at the spatial node of its parent, above the element's transform.
    fn element_with_own_effect(
        tree: &mut VisualContextTree,
        transformed: ContextRef,
        parent: ContextRef,
        opacity: f32,
    ) -> ContextRef {
        let effect = tree.append_effect(
            effects(opacity, CompositingAndBlendingOperator::Normal),
            parent.effect,
            parent.spatial,
            parent.clip,
        );
        ContextRef { effect, ..transformed }
    }

    fn space_names(stack: &LayerStack) -> Vec<String> {
        (0..stack.spaces.len())
            .map(|index| stack.name_of_space(RasterSpaceIndex(index), &NodeNamesFromIndices))
            .collect()
    }

    #[test]
    fn a_spinner_is_rastered_in_its_own_space_and_reaches_the_circle_it_turns_in() {
        let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
        let spinner = transformed_context(&mut tree, viewport, FloatRect::new(50.0, 50.0, 40.0, 40.0));
        tree.set_visual_animations(vec![spinning(spinner.spatial)]);
        let stack = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 100, 100), GREEN), viewport)
            .with(fill(IntRect::new(50, 50, 40, 40), GREEN), spinner)
            .with(fill(IntRect::new(0, 100, 100, 100), GREEN), viewport)
            .stack(&tree, &[]);
        assert_eq!(space_names(&stack), ["viewport", "animated@s1 in viewport"]);
        assert_eq!(
            kinds_of_layers_from_base_up_that_may_hold_content(&stack),
            [
                "base viewport band 0",
                "painted animated@s1 in viewport band 0",
                "painted viewport band 0 above band 0 of animated@s1 in viewport"
            ]
        );
        assert_eq!(
            stack.swept_reach_in_parent_space_by_space_index[1],
            Some(IntRect::new(34, 34, 72, 72))
        );
    }

    // A page inside an isolated wrapper, with a fixed box and a spinner in it.
    #[test]
    fn a_group_holding_content_of_several_spaces_keeps_them_around_a_spinner_inside_it() {
        let (mut tree, viewport, document) = tree_with_document_scroll();
        let wrapper = tree.append_effect_node_under(document, effects(0.99, CompositingAndBlendingOperator::Normal));
        let fixed_box = ContextRef {
            effect: wrapper.effect,
            ..viewport
        };
        let spinner = transformed_context(&mut tree, wrapper, FloatRect::new(300.0, 300.0, 40.0, 40.0));
        tree.set_visual_animations(vec![spinning(spinner.spatial)]);
        let list = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), wrapper)
            .with(fill(IntRect::new(300, 300, 40, 40), GREEN), spinner)
            .with(fill(IntRect::new(0, 0, 800, 50), GREEN), fixed_box);
        let planned = layers_planned_alike_for_every_sample(&list, &tree);
        assert!(planned.placement.animations_without_layers.is_empty());
        assert!(
            whys_of_composited_effects(&planned)
                .contains(&(wrapper.effect, WhyEffectIsComposited::ItHoldsContentOfSeveralSpaces))
        );
        let lines: Vec<String> = planned
            .layer_lines(
                &WindowServerPresentationOfMedia::default(),
                &NodeNamesFromIndices,
                |_| None,
            )
            .into_iter()
            .map(|(_, line)| line)
            .collect();
        assert_eq!(
            lines,
            [
                "[L0] base space=viewport",
                "[L2] painted space=document inside e1",
                "[L3] painted space=animated@s2 in document inside e1",
                "[L5] painted space=viewport inside e1 above band 0 of document inside e1",
                "composited e1 space=viewport: it holds content of several spaces"
            ]
        );
    }

    #[test]
    fn a_spinner_under_a_scale_far_below_the_smallest_raster_scale_is_rastered_at_the_smallest() {
        let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
        let shrunk = transformed_context(&mut tree, viewport, FloatRect::new(0.0, 0.0, 100.0, 100.0));
        let SpatialData::Transform(shrinking) = &mut tree.spatial_nodes[shrunk.spatial.0 as usize].data else {
            unreachable!()
        };
        shrinking.matrix = libgfx_rust::scale_matrix(0.001, 0.001, 1.0);
        let spinner = transformed_context(&mut tree, shrunk, FloatRect::new(50.0, 50.0, 40.0, 40.0));
        tree.set_visual_animations(vec![spinning(spinner.spatial)]);
        let stack = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 100, 100), GREEN), viewport)
            .with(fill(IntRect::new(50, 50, 40, 40), GREEN), spinner)
            .stack(&tree, &[]);
        let raster_scales: Vec<f32> = stack
            .spaces
            .iter()
            .filter_map(|space| match space.anchor_kind {
                RasterSpaceAnchorKind::AnimatedTransform { raster_scale } => Some(raster_scale),
                _ => None,
            })
            .collect();
        assert_eq!(raster_scales, [1.0 / 64.0]);
    }

    #[test]
    fn samples_of_a_spinner_leave_its_space_and_raster_geometry_alike() {
        let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
        let spinner = transformed_context(&mut tree, viewport, FloatRect::new(50.0, 50.0, 40.0, 40.0));
        tree.set_visual_animations(vec![spinning(spinner.spatial)]);
        let list = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 100, 100), GREEN), viewport)
            .with(fill(IntRect::new(50, 50, 40, 40), GREEN), spinner);
        let geometry_of_spinner_space_at = |time_ns: i64| {
            let sample = tree.with_visual_animation_samples(time_ns);
            let (_, geometries) =
                spaces_with_geometries(&sample, &RasterSpaceAnchorsOfTree::of(&sample), &[], VIEWPORT);
            geometries[1].raster.world.draw_matrix(spinner.spatial)
        };
        assert_eq!(
            geometry_of_spinner_space_at(0),
            geometry_of_spinner_space_at(250_000_000)
        );
        let at_start = list.planned(&tree.with_visual_animation_samples(0), &[]);
        let a_quarter_in = list.planned(&tree.with_visual_animation_samples(250_000_000), &[]);
        assert_eq!(at_start, a_quarter_in);
    }

    #[test]
    fn an_unclipped_slide_reaches_where_its_keyframes_take_it() {
        let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
        let slider = transformed_context(&mut tree, viewport, FloatRect::new(0.0, 0.0, 50.0, 50.0));
        tree.set_visual_animations(vec![sliding(slider.spatial)]);
        let stack = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 50, 50), GREEN), slider)
            .with(fill(IntRect::new(0, 50, 100, 100), GREEN), viewport)
            .stack(&tree, &[]);
        assert_eq!(space_names(&stack), ["viewport", "animated@s1 in viewport"]);
        assert_eq!(
            stack.swept_reach_in_parent_space_by_space_index[1],
            Some(IntRect::new(0, 0, 356, 56))
        );
    }

    #[test]
    fn a_slide_within_a_rounded_clip_is_composited_within_it() {
        let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
        let track = clipped_context(
            &mut tree,
            viewport,
            IntRect::new(0, 0, 200, 50),
            libgfx_rust::CornerRadii::uniform(10),
        );
        let slider = transformed_context(&mut tree, track, FloatRect::new(0.0, 0.0, 50.0, 50.0));
        tree.set_visual_animations(vec![sliding(slider.spatial)]);
        let planned = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 200, 50), GREEN), track)
            .with(fill(IntRect::new(0, 0, 50, 50), GREEN), slider)
            .planned(&tree, &[]);
        let lines: Vec<String> = planned
            .layer_lines(
                &WindowServerPresentationOfMedia::default(),
                &NodeNamesFromIndices,
                |_| None,
            )
            .into_iter()
            .map(|(_, line)| line)
            .collect();
        assert_eq!(
            lines,
            [
                "[L0] base space=viewport",
                "[L1] painted space=animated@s1 in viewport composited within [0,0 200x50] radii=(10,10,10,10) of viewport"
            ]
        );
    }

    #[test]
    fn a_skew_or_an_overshooting_slide_stays_in_the_tiles_around_it() {
        use crate::host::FfiVisualAnimationTransformOperationKind as Kind;
        let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
        let skewed = transformed_context(&mut tree, viewport, FloatRect::new(0.0, 0.0, 50.0, 50.0));
        let overshooting = transformed_context(&mut tree, viewport, FloatRect::new(0.0, 100.0, 50.0, 50.0));
        let mut overshooting_slide = sliding(overshooting.spatial);
        overshooting_slide.keyframes[0].easing = crate::easing::Easing::CubicBezier {
            x1: 0.3,
            y1: 1.6,
            x2: 0.6,
            y2: 1.0,
        };
        tree.set_visual_animations(vec![
            transform_animation_of(skewed.spatial, Kind::SkewX, 0.0, 0.3),
            overshooting_slide,
        ]);
        let planned = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 50, 50), GREEN), skewed)
            .with(fill(IntRect::new(0, 100, 50, 50), GREEN), overshooting)
            .planned(&tree, &[]);
        assert_eq!(
            kinds_of_layers_from_base_up_that_may_hold_content(&planned.stack),
            ["base viewport band 0"]
        );
        assert_eq!(
            planned.placement.animations_without_layers,
            [
                (
                    AnimatedNode::Transform(skewed.spatial),
                    WhyAnimationHasNoLayer::NothingBoundsWhereItMoves
                ),
                (
                    AnimatedNode::Transform(overshooting.spatial),
                    WhyAnimationHasNoLayer::NothingBoundsWhereItMoves
                )
            ]
        );
    }

    #[test]
    fn a_spinner_with_its_own_opacity_is_composited_inside_it() {
        let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
        let spinner = transformed_context(&mut tree, viewport, FloatRect::new(50.0, 50.0, 40.0, 40.0));
        let translucent_spinner = element_with_own_effect(&mut tree, spinner, viewport, 0.5);
        tree.set_visual_animations(vec![spinning(spinner.spatial)]);
        let planned = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 100, 100), GREEN), viewport)
            .with(fill(IntRect::new(50, 50, 40, 40), GREEN), translucent_spinner)
            .planned(&tree, &[]);
        assert_eq!(
            kinds_of_layers_from_base_up_that_may_hold_content(&planned.stack),
            ["base viewport band 0", "painted animated@s1 in viewport band 0"]
        );
        assert_eq!(
            whys_of_composited_effects(&planned),
            [(
                translucent_spinner.effect,
                WhyEffectIsComposited::ItAppliesOverATransformTheCompositorAnimates
            )]
        );
    }

    #[test]
    fn an_element_that_fades_and_spins_is_composited_through_both() {
        let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
        let spinner = transformed_context(&mut tree, viewport, FloatRect::new(50.0, 50.0, 40.0, 40.0));
        let fading_spinner = element_with_own_effect(&mut tree, spinner, viewport, 1.0);
        tree.set_visual_animations(vec![
            spinning(spinner.spatial),
            opacity_animation_of(fading_spinner.effect, 0.2, 1.0),
        ]);
        let planned = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 100, 100), GREEN), viewport)
            .with(fill(IntRect::new(50, 50, 40, 40), GREEN), fading_spinner)
            .planned(&tree, &[]);
        assert_eq!(
            kinds_of_layers_from_base_up_that_may_hold_content(&planned.stack),
            ["base viewport band 0", "painted animated@s1 in viewport band 0"]
        );
        assert_eq!(
            whys_of_composited_effects(&planned),
            // The fading effect pushes a layer at every sample, so it applies over the spinner at every sample.
            [(
                fading_spinner.effect,
                WhyEffectIsComposited::ItAppliesOverATransformTheCompositorAnimates
            )]
        );
    }

    #[test]
    fn a_spinner_in_a_translucent_group_with_siblings_is_placed_with_the_group() {
        let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
        let group = tree.append_effect_node_under(viewport, effects(0.5, CompositingAndBlendingOperator::Normal));
        let spinner = transformed_context(&mut tree, group, FloatRect::new(50.0, 50.0, 40.0, 40.0));
        tree.set_visual_animations(vec![spinning(spinner.spatial)]);
        let stack = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 100, 100), GREEN), group)
            .with(fill(IntRect::new(50, 50, 40, 40), GREEN), spinner)
            .stack(&tree, &[]);
        assert_eq!(
            kinds_of_layers_from_base_up_that_may_hold_content(&stack),
            ["base viewport band 0"]
        );
    }

    #[test]
    fn a_spinner_over_an_earlier_canvas_stays_in_the_tiles_around_it() {
        let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
        let spinner = transformed_context(&mut tree, viewport, FloatRect::new(50.0, 50.0, 40.0, 40.0));
        tree.set_visual_animations(vec![spinning(spinner.spatial)]);
        let planned = ListOfOneChunk::default()
            .with(canvas(IntRect::new(0, 0, 200, 120), 1), viewport)
            .with(fill(IntRect::new(50, 50, 40, 40), GREEN), spinner)
            .planned(&tree, &[]);
        assert_eq!(
            planned.placement.animations_without_layers,
            [(
                AnimatedNode::Transform(spinner.spatial),
                WhyAnimationHasNoLayer::ItCanShowOverAnEarlierCanvasOrVideo
            )]
        );
    }

    #[test]
    fn a_spinner_holding_a_canvas_gives_the_canvas_a_layer_in_its_space() {
        let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
        let spinner = transformed_context(&mut tree, viewport, FloatRect::new(50.0, 50.0, 40.0, 40.0));
        tree.set_visual_animations(vec![spinning(spinner.spatial)]);
        let stack = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 100, 100), GREEN), viewport)
            .with(canvas(IntRect::new(50, 50, 40, 40), 1), spinner)
            .stack(&tree, &[]);
        assert_eq!(
            kinds_of_layers_from_base_up_that_may_hold_content(&stack),
            ["base viewport band 0", "canvas #0 animated@s1 in viewport"]
        );
    }

    // The cut of the band at the layer of the first drawing of `content`.
    fn cut_of_media<'a>(stack: &'a LayerStack, content: LiveContent) -> &'a BarrierCut {
        stack
            .bands
            .iter()
            .flat_map(|band| &band.cuts)
            .find(|cut| {
                matches!(stack.layers[cut.layer_above_cut.0 - 1].identity,
                    LayerIdentity::Media { content: layer_content, .. } if layer_content == content)
            })
            .expect("the media is a barrier of a band")
    }

    #[test]
    fn a_caret_gets_a_layer_of_its_own() {
        let (tree, _, document) = tree_with_document_scroll();
        let planned = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(caret(IntRect::new(20, 20, 2, 16)), document)
            .planned(&tree, &[]);
        assert!(planned.placement.media_without_layers.is_empty());
        assert_eq!(
            kinds_of_layers_from_base_up_that_may_hold_content(&planned.stack),
            ["base document band 0", "caret #0 document"]
        );
    }

    #[test]
    fn records_painted_after_a_caret_go_above_it_in_the_tiles_it_touches() {
        let (tree, _, document) = tree_with_document_scroll();
        let stack = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(caret(IntRect::new(20, 20, 2, 16)), document)
            .with(fill(IntRect::new(10, 10, 100, 40), GREEN), document)
            .with(fill(IntRect::new(10, 1500, 100, 40), GREEN), document)
            .stack(&tree, &[]);
        assert_eq!(
            kinds_of_layers_from_base_up_that_may_hold_content(&stack),
            [
                "base document band 0",
                "caret #0 document",
                "painted document band 0 above L2"
            ]
        );
        let layer_above_caret = stack
            .layers
            .iter()
            .find(|layer| {
                matches!(
                    layer.identity,
                    LayerIdentity::Painted {
                        above_barrier: Some(BarrierIdentity::Media {
                            content: LiveContent::Caret,
                            ..
                        }),
                        ..
                    }
                )
            })
            .expect("a layer is above the caret");
        assert!(
            crate::display_list::tiles::TileCoord::new(0, 0)
                .cell_rect()
                .contains_rect(layer_above_caret.reach),
            "only the records in the tiles the caret touches go above it"
        );
    }

    // Text painted after a caret that it does not overlap stays in the base layer, where it takes LCD antialiasing.
    #[test]
    fn records_painted_after_a_caret_that_cannot_show_where_it_can_stay_below_it() {
        let (tree, _, document) = tree_with_document_scroll();
        let stack = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(caret(IntRect::new(20, 20, 2, 16)), document)
            .with(fill(IntRect::new(10, 100, 100, 40), GREEN), document)
            .stack(&tree, &[]);
        assert_eq!(
            kinds_of_layers_from_base_up_that_may_hold_content(&stack),
            ["base document band 0", "caret #0 document"]
        );
        assert_eq!(cut_of_media(&stack, LiveContent::Caret).cut_reach, Reach::Nothing);
    }

    // A focused text field draws its focus ring after its caret, around it.
    #[test]
    fn a_focus_ring_around_a_caret_keeps_text_painted_after_both_below_the_caret() {
        let (tree, _, document) = tree_with_document_scroll();
        let focus_ring = FillRoundedRectRing {
            rect: IntRect::new(-2, -2, 74, 24),
            corner_radii: libgfx_rust::CornerRadii::uniform(4),
            top_width: 2,
            right_width: 2,
            bottom_width: 2,
            left_width: 2,
            color: GREEN,
        };
        let stack = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(caret(IntRect::new(19, 3, 1, 14)), document)
            .with(focus_ring, document)
            .with(fill(IntRect::new(0, 23, 24, 16), GREEN), document)
            .stack(&tree, &[]);
        assert_eq!(
            kinds_of_layers_from_base_up_that_may_hold_content(&stack),
            ["base document band 0", "caret #0 document"]
        );
    }

    #[test]
    fn a_focus_ring_whose_rounded_corner_can_show_over_a_caret_goes_above_it() {
        let (tree, _, document) = tree_with_document_scroll();
        let focus_ring = FillRoundedRectRing {
            rect: IntRect::new(-2, -2, 74, 24),
            corner_radii: libgfx_rust::CornerRadii::uniform(12),
            top_width: 2,
            right_width: 2,
            bottom_width: 2,
            left_width: 2,
            color: GREEN,
        };
        let stack = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(caret(IntRect::new(4, 3, 1, 14)), document)
            .with(focus_ring, document)
            .stack(&tree, &[]);
        assert_ne!(cut_of_media(&stack, LiveContent::Caret).cut_reach, Reach::Nothing);
    }

    #[test]
    fn carets_are_numbered_by_the_carets_painted_before_them() {
        let (tree, _, document) = tree_with_document_scroll();
        let stack = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(caret(IntRect::new(20, 20, 2, 16)), document)
            .with(caret(IntRect::new(20, 1000, 2, 16)), document)
            .stack(&tree, &[]);
        assert_eq!(
            kinds_of_layers_from_base_up_that_may_hold_content(&stack),
            ["base document band 0", "caret #0 document", "caret #1 document"]
        );
    }

    // Unlike a canvas, a caret does not get the effects around it composited.
    #[test]
    fn a_caret_inside_an_opacity_group_stays_in_the_tiles_around_it() {
        let (mut tree, _, document) = tree_with_document_scroll();
        let faded = tree.append_effect_node_under(document, effects(0.5, CompositingAndBlendingOperator::Normal));
        let planned = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(caret(IntRect::new(20, 20, 2, 16)), faded)
            .planned(&tree, &[]);
        assert!(planned.placement.placed_media.is_empty());
        assert_eq!(
            planned.placement.media_without_layers,
            [(
                LiveContent::Caret,
                WhyMediaHasNoLayer::InsideAnEffectThatPushesALayerAtReplay
            )]
        );
        assert!(planned.stack.composited_effects.is_empty());
    }

    #[test]
    fn a_scroll_bar_gets_a_layer_of_its_own() {
        let (tree, _, contexts) = tree_with_document_scroller(&[IntRect::new(100, 100, 300, 200)]);
        let (document, scroller) = (contexts[0], contexts[1]);
        let planned = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(fill(IntRect::new(100, 100, 300, 700), GREEN), scroller)
            .with(scroll_bar(scroller.spatial, IntRect::new(390, 100, 10, 200)), document)
            .planned(&tree, &[]);
        assert!(planned.placement.media_without_layers.is_empty());
        assert_eq!(
            kinds_of_layers_from_base_up_that_may_hold_content(&planned.stack),
            [
                "base document band 0",
                "painted scroller@s2 in document band 0",
                "vertical scroll bar #0 document"
            ]
        );
    }

    // Carets and scroll bars get layers however many canvases do, and past the most carets and scroll bars that get
    // layers, carets keep theirs.
    #[test]
    fn carets_and_scroll_bars_count_against_a_limit_of_their_own() {
        let (tree, _, contexts) = tree_with_document_scroller(&[IntRect::new(100, 100, 300, 200)]);
        let (document, scroller) = (contexts[0], contexts[1]);
        let canvas_count = 10;
        let mut list = ListOfOneChunk::default().with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document);
        for index in 0..canvas_count {
            list = list.with(
                canvas(IntRect::new(10 + index as i32 * 60, 1000, 50, 50), index as u64 + 1),
                document,
            );
        }
        for index in 0..MAXIMUM_LAYER_COUNT_OF_LIVE_CONTENT_DRAWN_BY_ITS_RECORD as i32 {
            list = list.with(
                scroll_bar(scroller.spatial, IntRect::new(10 + index * 12, 100, 10, 200)),
                document,
            );
        }
        let planned = list
            .with(caret(IntRect::new(20, 400, 2, 16)), document)
            .planned(&tree, &[]);
        let placed_count = |is_of_kind: fn(&LiveContent) -> bool| {
            planned
                .placement
                .placed_media
                .iter()
                .filter(|media| is_of_kind(&media.content))
                .count()
        };
        assert_eq!(
            placed_count(|content| matches!(content, LiveContent::Canvas(_))),
            canvas_count
        );
        assert_eq!(placed_count(|content| *content == LiveContent::Caret), 1);
        assert_eq!(
            placed_count(|content| matches!(content, LiveContent::ScrollBar { .. })),
            MAXIMUM_LAYER_COUNT_OF_LIVE_CONTENT_DRAWN_BY_ITS_RECORD - 1
        );
        assert_eq!(
            planned.placement.media_without_layers,
            [(
                LiveContent::ScrollBar {
                    scroll_node: scroller.spatial,
                    vertical: true
                },
                WhyMediaHasNoLayer::MoreMediaThanLayersFor
            )]
        );
    }

    // Canvases and nested contexts in turn, in rows, the first a canvas.
    fn page_of_canvases_and_nested_contexts_in_rows(media_count: usize) -> (VisualContextTree, ListOfOneChunk) {
        let (tree, _, document) = tree_with_document_scroll();
        let mut list = ListOfOneChunk::default().with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document);
        for (index, content) in canvases_and_nested_contexts_in_turn(media_count)
            .into_iter()
            .enumerate()
        {
            let rect = IntRect::new((index % 20) as i32 * 40, (index / 20) as i32 * 40, 30, 30);
            list = match content {
                LiveContent::Canvas(CanvasId(id)) => list.with(canvas(rect, id), document),
                LiveContent::CompositedContext(CompositorContextId(id)) => {
                    list.with(nested_context(rect, id, document), document)
                }
                _ => unreachable!(),
            };
        }
        (tree, list)
    }

    fn canvases_and_nested_contexts_in_turn(media_count: usize) -> Vec<LiveContent> {
        (1..=media_count as u64)
            .map(|id| {
                if id % 2 == 1 {
                    LiveContent::Canvas(CanvasId(id))
                } else {
                    LiveContent::CompositedContext(CompositorContextId(id))
                }
            })
            .collect()
    }

    fn media_with_layers(planned: &PlannedLayers) -> Vec<LiveContent> {
        planned
            .placement
            .placed_media
            .iter()
            .map(|media| media.content)
            .collect()
    }

    #[test]
    fn every_canvas_and_nested_context_gets_a_layer_however_many_a_page_paints() {
        let (tree, list) = page_of_canvases_and_nested_contexts_in_rows(100);
        let planned = list.planned(&tree, &[]);
        assert_eq!(planned.stack.why_list_is_one_layer, None);
        assert!(planned.placement.media_without_layers.is_empty());
        assert_eq!(media_with_layers(&planned), canvases_and_nested_contexts_in_turn(100));
    }

    // Each canvas, video or nested context with a layer cuts its band once. Where every one of them having a layer would
    // cut a band more times than the compositor decodes, those painted first keep layers, up to as many as a band leaves
    // cuts for, and the rest of the list stays layered.
    #[test]
    fn canvases_and_nested_contexts_past_the_cuts_a_band_leaves_for_them_get_no_layers() {
        let media_count = CANVAS_VIDEO_AND_NESTED_CONTEXT_LAYERS_A_BAND_LEAVES_CUTS_FOR + 100;
        let (tree, list) = page_of_canvases_and_nested_contexts_in_rows(media_count);
        let planned = list.planned(&tree, &[]);
        assert_eq!(planned.stack.why_list_is_one_layer, None);
        assert_eq!(
            media_with_layers(&planned),
            canvases_and_nested_contexts_in_turn(CANVAS_VIDEO_AND_NESTED_CONTEXT_LAYERS_A_BAND_LEAVES_CUTS_FOR)
        );
        assert_eq!(planned.placement.media_without_layers.len(), 100);
        assert!(
            planned
                .placement
                .media_without_layers
                .iter()
                .all(|(_, why)| *why == WhyMediaHasNoLayer::MoreMediaThanLayersFor)
        );
    }

    // A scroll bar of the viewport is drawn over the document in the space of the viewport, and does not scroll with
    // the document.
    #[test]
    fn a_scroll_bar_of_the_viewport_gets_a_layer_of_its_own() {
        let (tree, viewport, document) = tree_with_document_scroll();
        let planned = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(scroll_bar(document.spatial, IntRect::new(790, 0, 10, 600)), viewport)
            .planned(&tree, &[]);
        assert!(planned.placement.media_without_layers.is_empty());
        assert_eq!(
            kinds_of_layers_from_base_up_that_may_hold_content(&planned.stack),
            ["base document band 0", "vertical scroll bar #0 viewport"]
        );
    }

    // The scroll bars of the viewport are recorded after all else, and past the most carets and scroll bars that get
    // layers, they keep theirs before the scroll bars of other scrollers.
    #[test]
    fn the_scroll_bars_of_the_viewport_keep_their_layers_before_those_of_other_scrollers() {
        let (tree, viewport, contexts) = tree_with_document_scroller(&[IntRect::new(100, 100, 300, 200)]);
        let (document, scroller) = (contexts[0], contexts[1]);
        let mut list = ListOfOneChunk::default().with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document);
        for index in 0..MAXIMUM_LAYER_COUNT_OF_LIVE_CONTENT_DRAWN_BY_ITS_RECORD as i32 {
            list = list.with(
                scroll_bar(scroller.spatial, IntRect::new(10 + index * 12, 100, 10, 200)),
                document,
            );
        }
        let planned = list
            .with(scroll_bar(document.spatial, IntRect::new(790, 0, 10, 600)), viewport)
            .planned(&tree, &[]);
        let scroll_bar_of_viewport = LiveContent::ScrollBar {
            scroll_node: document.spatial,
            vertical: true,
        };
        assert!(
            planned
                .placement
                .placed_media
                .iter()
                .any(|media| media.content == scroll_bar_of_viewport)
        );
        assert_eq!(
            planned.placement.media_without_layers,
            [(
                LiveContent::ScrollBar {
                    scroll_node: scroller.spatial,
                    vertical: true
                },
                WhyMediaHasNoLayer::MoreMediaThanLayersFor
            )]
        );
    }

    #[test]
    fn a_canvas_that_no_record_painted_after_it_overlaps_cuts_no_tile() {
        let (tree, _, document) = tree_with_document_scroll();
        let stack = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(canvas(IntRect::new(20, 20, 100, 100), 1), document)
            .with(fill(IntRect::new(20, 200, 100, 40), GREEN), document)
            .stack(&tree, &[]);
        assert_eq!(
            kinds_of_layers_from_base_up_that_may_hold_content(&stack),
            ["base document band 0", "canvas #0 document"]
        );
        assert_eq!(
            cut_of_media(&stack, LiveContent::Canvas(CanvasId(1))).cut_reach,
            Reach::Nothing
        );
    }

    #[test]
    fn a_fade_over_an_earlier_canvas_stays_in_the_tiles_around_it() {
        let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
        let controls = tree.append_effect_node_under(viewport, effects(1.0, CompositingAndBlendingOperator::Normal));
        tree.set_visual_animations(vec![opacity_animation_of(controls.effect, 0.2, 1.0)]);
        let planned = ListOfOneChunk::default()
            .with(canvas(IntRect::new(0, 0, 200, 120), 1), viewport)
            .with(fill(IntRect::new(0, 80, 200, 40), GREEN), controls)
            .planned(&tree, &[]);
        assert!(planned.stack.composited_effects.is_empty());
        assert_eq!(
            planned.placement.animations_without_layers,
            [(
                AnimatedNode::Effect(controls.effect),
                WhyAnimationHasNoLayer::ItCanShowOverAnEarlierCanvasOrVideo
            )]
        );
    }

    // Before the animation starts, at its first keyframe, between its keyframes, at its last keyframe and after its
    // end, for an animation that `played_once_after_a_delay` gives.
    const SAMPLE_TIMES_NS: [i64; 6] = [0, 200_000_000, 450_000_000, 700_000_000, 1_200_000_000, 2_000_000_000];

    fn played_once_after_a_delay(
        animation: crate::visual_animation::VisualAnimation,
    ) -> crate::visual_animation::VisualAnimation {
        crate::visual_animation::VisualAnimation {
            timing: crate::animation_timing::AnimationEffectTiming {
                start_delay: crate::animation_timing::TimeValue::milliseconds(200.0),
                iteration_count: 1.0,
                ..animation.timing
            },
            ..animation
        }
    }

    fn layers_planned_alike_for_every_sample(list: &ListOfOneChunk, tree: &VisualContextTree) -> PlannedLayers {
        let planned = list.planned(tree, &[]);
        for time_ns in SAMPLE_TIMES_NS {
            assert!(
                list.planned(&tree.with_visual_animation_samples(time_ns), &[]) == planned,
                "the plan of the sample at {time_ns} ns differs from the plan of the tree"
            );
        }
        planned
    }

    fn filter_function(
        kind: crate::filter_bytes::FfiFilterFunctionKind,
        amount: f32,
    ) -> crate::filter_bytes::FfiFilterFunction {
        crate::filter_bytes::FfiFilterFunction {
            kind,
            amount,
            offset_x: 0.0,
            offset_y: 0.0,
            color: Color::TRANSPARENT,
            color_operation: libgfx_rust::ColorFilterType::Grayscale,
        }
    }

    fn grayscale(amount: f32) -> crate::filter_bytes::FfiFilterFunction {
        filter_function(crate::filter_bytes::FfiFilterFunctionKind::Color, amount)
    }

    fn blur(radius: f32) -> crate::filter_bytes::FfiFilterFunction {
        filter_function(crate::filter_bytes::FfiFilterFunctionKind::Blur, radius)
    }

    fn filter_bytes_of(functions: &[crate::filter_bytes::FfiFilterFunction]) -> Option<std::sync::Arc<Vec<u8>>> {
        crate::filter_bytes::filter_functions_graph(functions.iter().copied().map(libgfx_rust::filter::Filter::from))
            .map(|graph| std::sync::Arc::new(graph.serialize()))
    }

    fn filter_animation_of(
        effect: EffectNodeIndex,
        from: Vec<crate::filter_bytes::FfiFilterFunction>,
        to: Vec<crate::filter_bytes::FfiFilterFunction>,
    ) -> crate::visual_animation::VisualAnimation {
        let keyframe = |offset: f64, functions| crate::visual_animation::VisualAnimationKeyframe {
            offset,
            easing: crate::easing::Easing::default(),
            value: crate::visual_animation::VisualAnimationValue::Filter(functions),
        };
        animation_of(
            crate::host::FfiVisualAnimationTargetKind::Filter,
            effect.0,
            vec![keyframe(0.0, from), keyframe(1.0, to)],
        )
    }

    fn element_with_filter(
        tree: &mut VisualContextTree,
        parent: ContextRef,
        filter: Option<std::sync::Arc<Vec<u8>>>,
    ) -> ContextRef {
        tree.append_effect_node_under(
            parent,
            EffectNodeData::Effects(EffectsData {
                isolation: false,
                opacity: 1.0,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter,
                backdrop_filter: None,
            }),
        )
    }

    #[derive(Clone, Copy, Debug)]
    enum Animated {
        OpacityFromOne,
        ColorFilter,
        BlurFilter,
        BlurFilterFromNone,
        Translation,
        Rotation,
        Scale,
        RotationOutOfPlane,
    }

    #[derive(Clone, Copy, Debug)]
    enum UnderAnimatedNode {
        Fill,
        Canvas,
        BackdropFilter,
    }

    const ELEMENT: IntRect = IntRect::new(50, 50, 40, 40);
    const WRAPPER_AROUND_ELEMENT: IntRect = IntRect::new(0, 0, 800, 600);
    const BLENDING_ELEMENT_IN_ANOTHER_CELL: IntRect = IntRect::new(600, TILE_HEIGHT_IN_DEVICE_PIXELS + 10, 150, 60);

    // An element whose own value equals that of the first keyframe of its animation, between two fills of the page.
    fn page_with_animated_element(
        animated: Animated,
        under_animated_node: UnderAnimatedNode,
    ) -> (VisualContextTree, ListOfOneChunk, ContextRef) {
        use crate::host::FfiVisualAnimationTransformOperationKind as Kind;
        let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
        let (element, animation) = match animated {
            Animated::OpacityFromOne => {
                let element =
                    tree.append_effect_node_under(viewport, effects(1.0, CompositingAndBlendingOperator::Normal));
                (element, opacity_animation_of(element.effect, 1.0, 0.0))
            }
            Animated::ColorFilter => {
                let element = element_with_filter(&mut tree, viewport, filter_bytes_of(&[grayscale(0.0)]));
                let animation = filter_animation_of(element.effect, vec![grayscale(0.0)], vec![grayscale(1.0)]);
                (element, animation)
            }
            Animated::BlurFilter => {
                let element = element_with_filter(&mut tree, viewport, filter_bytes_of(&[blur(0.0)]));
                let animation = filter_animation_of(element.effect, vec![blur(0.0)], vec![blur(4.0)]);
                (element, animation)
            }
            Animated::BlurFilterFromNone => {
                let element = element_with_filter(&mut tree, viewport, None);
                let animation = filter_animation_of(element.effect, Vec::new(), vec![blur(4.0)]);
                (element, animation)
            }
            Animated::Translation | Animated::Rotation | Animated::Scale | Animated::RotationOutOfPlane => {
                let element = transformed_context(&mut tree, viewport, ELEMENT.to_float());
                let (kind, from, to) = match animated {
                    Animated::Translation => (Kind::TranslateX, 0.0, 100.0),
                    Animated::Rotation => (Kind::Rotate, 0.0, 360.0),
                    Animated::Scale => (Kind::Scale, 1.0, 2.0),
                    _ => (Kind::RotateX, 0.0, 180.0),
                };
                (element, transform_animation_of(element.spatial, kind, from, to))
            }
        };
        tree.set_visual_animations(vec![played_once_after_a_delay(animation)]);
        let list = ListOfOneChunk::default().with(fill(IntRect::new(0, 0, 200, 200), GREEN), viewport);
        let list = match under_animated_node {
            UnderAnimatedNode::Fill => list.with(fill(ELEMENT, GREEN), element),
            UnderAnimatedNode::Canvas => list.with(canvas(ELEMENT, 1), element),
            UnderAnimatedNode::BackdropFilter => {
                let glass = frosted_glass(&mut tree, element, ELEMENT);
                list.with(fill(ELEMENT, GREEN), glass)
            }
        };
        let list = list.with(fill(IntRect::new(0, 150, 200, 50), GREEN), viewport);
        (tree, list, element)
    }

    #[test]
    fn every_sample_of_an_animation_starting_at_the_value_of_the_tree_is_planned_as_the_tree_is() {
        for animated in [
            Animated::OpacityFromOne,
            Animated::ColorFilter,
            Animated::BlurFilter,
            Animated::BlurFilterFromNone,
            Animated::Translation,
            Animated::Rotation,
            Animated::Scale,
            Animated::RotationOutOfPlane,
        ] {
            for under_animated_node in [
                UnderAnimatedNode::Fill,
                UnderAnimatedNode::Canvas,
                UnderAnimatedNode::BackdropFilter,
            ] {
                let (tree, list, _) = page_with_animated_element(animated, under_animated_node);
                let stack = layers_planned_alike_for_every_sample(&list, &tree).stack;
                assert!(
                    stack.why_list_is_one_layer.is_none(),
                    "{animated:?} over {under_animated_node:?} is one layer: {:?}",
                    stack.why_list_is_one_layer
                );
            }
        }
    }

    fn whys_of_composited_effects(planned: &PlannedLayers) -> Vec<(EffectNodeIndex, WhyEffectIsComposited)> {
        planned
            .composited_effects_with_why_and_backdrop_read_reach()
            .map(|(composited, why, _)| (composited.effect, why))
            .collect()
    }

    #[test]
    fn an_opacity_animation_starting_opaque_is_composited_at_every_sample() {
        let (tree, list, element) = page_with_animated_element(Animated::OpacityFromOne, UnderAnimatedNode::Fill);
        assert!(
            tree.with_visual_animation_samples(0)
                .effect_pushes_layer_at_replay(element.effect)
        );
        let planned = layers_planned_alike_for_every_sample(&list, &tree);
        assert_eq!(
            whys_of_composited_effects(&planned),
            [(element.effect, WhyEffectIsComposited::TheCompositorAnimatesIt)]
        );
    }

    #[test]
    fn a_backdrop_filter_in_an_element_whose_opacity_is_animated_reads_the_layer_of_the_element() {
        let (tree, list, _) = page_with_animated_element(Animated::OpacityFromOne, UnderAnimatedNode::BackdropFilter);
        let planned = layers_planned_alike_for_every_sample(&list, &tree);
        assert!(
            planned
                .placement
                .placed_effects
                .iter()
                .all(|placed| placed.why_composited_regardless_of_layer_content.is_some())
        );
    }

    // An element whose opacity is animated in a group, under a later element of the group that blends with what is
    // below it over `blending_element_rect`.
    fn group_with_animated_element_under_blending_element(
        blending_element_rect: IntRect,
    ) -> (VisualContextTree, ListOfOneChunk, ContextRef) {
        let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
        let group = tree.append_effect_node_under(viewport, effects(0.99, CompositingAndBlendingOperator::Normal));
        let element = tree.append_effect_node_under(group, effects(1.0, CompositingAndBlendingOperator::Normal));
        let blending_element =
            tree.append_effect_node_under(group, effects(1.0, CompositingAndBlendingOperator::Overlay));
        tree.set_visual_animations(vec![played_once_after_a_delay(opacity_animation_of(
            element.effect,
            1.0,
            0.0,
        ))]);
        let list = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 200, 200), GREEN), viewport)
            .with(fill(ELEMENT, GREEN), element)
            .with(fill(blending_element_rect, GREEN), blending_element);
        (tree, list, element)
    }

    #[test]
    fn an_animation_a_later_effect_reads_as_backdrop_has_no_layer() {
        let (tree, list, element) = group_with_animated_element_under_blending_element(IntRect::new(0, 0, 200, 200));
        let planned = layers_planned_alike_for_every_sample(&list, &tree);
        assert!(planned.stack.composited_effects.is_empty());
        assert_eq!(
            planned.placement.animations_without_layers,
            [(
                AnimatedNode::Effect(element.effect),
                WhyAnimationHasNoLayer::ALaterEffectReadsItAsBackdrop
            )]
        );

        // Where the blending element reads nothing where the animated element reaches, the animation keeps its layer.
        let (tree, list, element) = group_with_animated_element_under_blending_element(IntRect::new(0, 2000, 200, 200));
        let planned = layers_planned_alike_for_every_sample(&list, &tree);
        assert!(planned.placement.animations_without_layers.is_empty());
        assert!(
            whys_of_composited_effects(&planned)
                .contains(&(element.effect, WhyEffectIsComposited::TheCompositorAnimatesIt))
        );
    }

    #[test]
    fn an_animation_in_a_wrapper_a_later_effect_reads_as_backdrop_has_no_layer() {
        // The blending element reads nothing where the animated element reaches, but it reads where the wrapper around
        // it reaches, which a layer of the animation would composite.
        let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
        let group = tree.append_effect_node_under(viewport, effects(0.99, CompositingAndBlendingOperator::Normal));
        let wrapper = tree.append_effect_node_under(group, effects(0.99, CompositingAndBlendingOperator::Normal));
        let element = tree.append_effect_node_under(wrapper, effects(1.0, CompositingAndBlendingOperator::Normal));
        let blending_element =
            tree.append_effect_node_under(group, effects(1.0, CompositingAndBlendingOperator::Overlay));
        tree.set_visual_animations(vec![played_once_after_a_delay(opacity_animation_of(
            element.effect,
            1.0,
            0.0,
        ))]);
        let list = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 200, 200), GREEN), viewport)
            .with(fill(WRAPPER_AROUND_ELEMENT, GREEN), wrapper)
            .with(fill(ELEMENT, GREEN), element)
            .with(fill(BLENDING_ELEMENT_IN_ANOTHER_CELL, GREEN), blending_element);
        let planned = layers_planned_alike_for_every_sample(&list, &tree);
        assert!(planned.stack.composited_effects.is_empty());
        assert_eq!(
            planned.placement.animations_without_layers,
            [(
                AnimatedNode::Effect(element.effect),
                WhyAnimationHasNoLayer::ALaterEffectReadsItAsBackdrop
            )]
        );
    }

    #[test]
    fn an_animation_in_a_wrapper_that_pushes_no_layer_keeps_its_layer() {
        // The wrapper replays straight into its group, so a layer of the animation composites only the animated element,
        // which the blending element reads nothing of.
        let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
        let group = tree.append_effect_node_under(viewport, effects(0.99, CompositingAndBlendingOperator::Normal));
        let wrapper = tree.append_effect_node_under(group, effects(1.0, CompositingAndBlendingOperator::Normal));
        let element = tree.append_effect_node_under(wrapper, effects(1.0, CompositingAndBlendingOperator::Normal));
        let blending_element =
            tree.append_effect_node_under(group, effects(1.0, CompositingAndBlendingOperator::Overlay));
        tree.set_visual_animations(vec![played_once_after_a_delay(opacity_animation_of(
            element.effect,
            1.0,
            0.0,
        ))]);
        let list = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 200, 200), GREEN), viewport)
            .with(fill(WRAPPER_AROUND_ELEMENT, GREEN), wrapper)
            .with(fill(ELEMENT, GREEN), element)
            .with(fill(BLENDING_ELEMENT_IN_ANOTHER_CELL, GREEN), blending_element);
        let planned = layers_planned_alike_for_every_sample(&list, &tree);
        assert!(planned.placement.animations_without_layers.is_empty());
        assert!(
            whys_of_composited_effects(&planned)
                .contains(&(element.effect, WhyEffectIsComposited::TheCompositorAnimatesIt))
        );
    }

    #[test]
    fn an_animation_under_a_blending_element_composited_for_its_canvas_keeps_its_layer() {
        let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
        let group = tree.append_effect_node_under(viewport, effects(0.99, CompositingAndBlendingOperator::Normal));
        let element = tree.append_effect_node_under(group, effects(1.0, CompositingAndBlendingOperator::Normal));
        let blending_element =
            tree.append_effect_node_under(group, effects(1.0, CompositingAndBlendingOperator::Overlay));
        tree.set_visual_animations(vec![played_once_after_a_delay(opacity_animation_of(
            element.effect,
            1.0,
            0.0,
        ))]);
        let list = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 200, 200), GREEN), viewport)
            .with(fill(ELEMENT, GREEN), element)
            .with(canvas(IntRect::new(0, 0, 200, 200), 1), blending_element);
        let planned = layers_planned_alike_for_every_sample(&list, &tree);
        assert!(planned.placement.animations_without_layers.is_empty());
        let whys = whys_of_composited_effects(&planned);
        assert!(whys.contains(&(element.effect, WhyEffectIsComposited::TheCompositorAnimatesIt)));
        assert!(whys.iter().any(|&(effect, _)| effect == blending_element.effect));
    }

    #[test]
    fn a_color_filter_animation_is_composited_and_a_blur_animation_is_not() {
        let (tree, list, element) = page_with_animated_element(Animated::ColorFilter, UnderAnimatedNode::Fill);
        let planned = layers_planned_alike_for_every_sample(&list, &tree);
        assert_eq!(
            whys_of_composited_effects(&planned),
            [(element.effect, WhyEffectIsComposited::TheCompositorAnimatesIt)]
        );

        let (tree, list, element) = page_with_animated_element(Animated::BlurFilter, UnderAnimatedNode::Fill);
        let planned = layers_planned_alike_for_every_sample(&list, &tree);
        assert!(planned.stack.composited_effects.is_empty());
        assert_eq!(
            planned.placement.animations_without_layers,
            [(
                AnimatedNode::Effect(element.effect),
                WhyAnimationHasNoLayer::ItsFilterSpreadsWhatItFilters
            )]
        );
        assert_eq!(
            planned.placement.why_compositor_cannot_composite_by_effect_index[element.effect.0 as usize],
            None
        );

        // Also where the tree holds no filter, as the first keyframe gives none.
        let (tree, list, element) = page_with_animated_element(Animated::BlurFilterFromNone, UnderAnimatedNode::Fill);
        let planned = layers_planned_alike_for_every_sample(&list, &tree);
        assert_eq!(
            planned.placement.animations_without_layers,
            [(
                AnimatedNode::Effect(element.effect),
                WhyAnimationHasNoLayer::ItsFilterSpreadsWhatItFilters
            )]
        );
    }

    #[test]
    fn an_element_with_its_own_blur_animated_between_color_filters_is_planned_for_its_blur() {
        let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
        let element = element_with_filter(&mut tree, viewport, filter_bytes_of(&[blur(5.0)]));
        tree.set_visual_animations(vec![filter_animation_of(
            element.effect,
            vec![grayscale(0.0)],
            vec![grayscale(1.0)],
        )]);
        let planned = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 200, 200), GREEN), viewport)
            .with(fill(ELEMENT, GREEN), element)
            .planned(&tree, &[]);
        assert!(planned.stack.composited_effects.is_empty());
        assert_eq!(
            planned.placement.animations_without_layers,
            [(
                AnimatedNode::Effect(element.effect),
                WhyAnimationHasNoLayer::ItsFilterSpreadsWhatItFilters
            )]
        );
    }

    #[test]
    fn a_blur_animation_whose_easing_overshoots_reads_farther_than_is_known() {
        let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
        let element = element_with_filter(&mut tree, viewport, filter_bytes_of(&[blur(0.0)]));
        let mut animation = filter_animation_of(element.effect, vec![blur(0.0)], vec![blur(4.0)]);
        animation.keyframes[0].easing = crate::easing::Easing::CubicBezier {
            x1: 0.3,
            y1: 1.6,
            x2: 0.6,
            y2: 1.0,
        };
        tree.set_visual_animations(vec![animation]);
        assert_eq!(
            ListOfOneChunk::default()
                .with(fill(IntRect::new(0, 0, 200, 200), GREEN), viewport)
                .with(fill(ELEMENT, GREEN), element)
                .planned(&tree, &[])
                .placement
                .why_compositor_cannot_composite_by_effect_index[element.effect.0 as usize],
            Some(WhyCompositorCannotCompositeEffect::FilterReadsFartherThanTilesAreFollowed)
        );
    }

    #[test]
    fn transforms_that_stay_in_the_plane_get_spaces_and_one_that_leaves_it_keeps_its_canvas_from_a_layer() {
        for animated in [Animated::Translation, Animated::Rotation, Animated::Scale] {
            let (tree, list, element) = page_with_animated_element(animated, UnderAnimatedNode::Canvas);
            let planned = layers_planned_alike_for_every_sample(&list, &tree);
            assert_eq!(
                space_names(&planned.stack),
                ["viewport", &format!("animated@s{} in viewport", element.spatial.0)]
            );
            assert!(planned.placement.media_without_layers.is_empty());
        }
        let (tree, list, _) = page_with_animated_element(Animated::RotationOutOfPlane, UnderAnimatedNode::Canvas);
        let planned = layers_planned_alike_for_every_sample(&list, &tree);
        assert_eq!(space_names(&planned.stack), ["viewport"]);
        assert_eq!(
            planned.placement.media_without_layers,
            [(
                LiveContent::Canvas(CanvasId(1)),
                WhyMediaHasNoLayer::UnderTransformThatIsNot2DAffine
            )]
        );
    }

    #[test]
    fn a_backdrop_filter_moved_by_a_transform_animation_reads_anywhere() {
        let (tree, list, _) = page_with_animated_element(Animated::Translation, UnderAnimatedNode::BackdropFilter);
        let planned = layers_planned_alike_for_every_sample(&list, &tree);
        assert_eq!(space_names(&planned.stack), ["viewport"]);
        assert!(
            planned
                .placement
                .placed_effects
                .iter()
                .any(|placed| placed.backdrop_read_reach == Reach::Anywhere)
        );
    }

    #[test]
    fn a_plane_whose_animation_turns_it_out_of_its_sorting_context_is_sorted_by_depth_at_every_sample() {
        use crate::host::FfiVisualAnimationTransformOperationKind as Kind;
        let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
        let plane_of = |tree: &mut VisualContextTree, parent: SpatialNodeIndex, root: Option<SpatialNodeIndex>| {
            tree.append_spatial(
                SpatialData::Transform(TransformData {
                    matrix: FloatMatrix4x4::identity(),
                    origin: FloatPoint { x: 70.0, y: 70.0 },
                    sorting_context_root_index: root,
                    flattens_inherited_transform: false,
                    role: TransformDataRole::CssTransform,
                    synthetic_plane: false,
                    establishes_sorting_context: root.is_none(),
                    bounds: ELEMENT.to_float(),
                }),
                parent,
            )
        };
        let root = plane_of(&mut tree, viewport.spatial, None);
        let still_plane = plane_of(&mut tree, root, Some(root));
        let turning_plane = plane_of(&mut tree, root, Some(root));
        tree.set_visual_animations(vec![played_once_after_a_delay(transform_animation_of(
            turning_plane,
            Kind::RotateY,
            0.0,
            180.0,
        ))]);
        let list = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 200, 200), GREEN), viewport)
            .with(
                fill(ELEMENT, GREEN),
                ContextRef {
                    spatial: still_plane,
                    ..viewport
                },
            )
            .with(
                fill(ELEMENT, GREEN),
                ContextRef {
                    spatial: turning_plane,
                    ..viewport
                },
            );
        // Replay sorts the planes by depth where they leave the plane of their context in the frame.
        assert!(!tree.with_visual_animation_samples(0).sorts_planes_by_depth());
        assert!(tree.with_visual_animation_samples(700_000_000).sorts_planes_by_depth());
        let storage = DisplayListStorage::of_builder(&list.builder);
        for time_ns in SAMPLE_TIMES_NS {
            let sample = tree.with_visual_animation_samples(time_ns);
            assert!(sample.may_sort_planes_by_depth());
            assert_eq!(
                sample.roots_of_contexts_sorting_planes_by_depth()[turning_plane.0 as usize],
                Some(root)
            );
            let run_groups = storage.run_groups(&sample);
            assert!(!run_groups.run_is_sorted_by_depth(0));
            assert!(run_groups.run_is_sorted_by_depth(1) && run_groups.run_is_sorted_by_depth(2));
        }
        let stack = layers_planned_alike_for_every_sample(&list, &tree).stack;
        assert!(stack.layering_obstacles.sorts_planes_by_depth);
        assert!(stack.why_list_is_one_layer.is_none());
    }

    #[test]
    fn a_rotation_out_of_the_plane_stays_in_it_only_where_it_keeps_its_angle() {
        use crate::display_list::animated_transform_spaces::transform_animation_stays_in_plane;
        use crate::host::FfiVisualAnimationTransformOperationKind as Kind;
        let node = SpatialNodeIndex(1);
        assert!(transform_animation_stays_in_plane(&transform_animation_of(
            node,
            Kind::Rotate,
            0.0,
            360.0
        )));
        assert!(transform_animation_stays_in_plane(&transform_animation_of(
            node,
            Kind::RotateX,
            0.0,
            0.0
        )));
        // Each keyframe is in the plane within the precision of a rotation matrix, but the angle changes.
        assert!(!transform_animation_stays_in_plane(&transform_animation_of(
            node,
            Kind::RotateX,
            0.0,
            1e-8
        )));
        assert!(!transform_animation_stays_in_plane(&transform_animation_of(
            node,
            Kind::RotateY,
            0.0,
            180.0
        )));
    }

    #[test]
    fn a_plan_holds_wherever_a_sticky_box_that_anchors_a_space_is_held() {
        use crate::visual_context::StickyData;
        let (mut tree, _, document) = tree_with_document_scroll();
        let mut sticky_data = StickyData::unconstrained(
            document.spatial,
            None,
            crate::visual_context::scroll_state::NO_SCROLL_STATE_SLOT,
            NodeSlotId::INVALID,
        );
        sticky_data.position_relative_to_scroller = FloatPoint { x: 0.0, y: 100.0 };
        sticky_data.border_box_size = libgfx_rust::FloatSize {
            width: 800.0,
            height: 50.0,
        };
        sticky_data.scrollport_size = libgfx_rust::FloatSize {
            width: 800.0,
            height: 600.0,
        };
        sticky_data.containing_block_region = FloatRect::new(0.0, 0.0, 800.0, 2000.0);
        sticky_data.needs_parent_offset_adjustment = true;
        sticky_data.inset_top = Some(0.0);
        let (_, highest_offset) = sticky_data.offset_range_without_sticky_ancestors();
        let sticky_node = tree.append_spatial(SpatialData::Sticky(sticky_data), document.spatial);
        let sticky = ContextRef {
            spatial: sticky_node,
            ..document
        };
        // The box comes over what the document paints after it only once the document is scrolled.
        let list = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(fill(IntRect::new(0, 100, 800, 50), GREEN), sticky)
            .with(canvas(IntRect::new(10, 110, 30, 30), 1), sticky)
            .with(fill(IntRect::new(0, 900, 800, 100), GREEN), document);
        let stack_with_box_held_at = |offset: FloatPoint| {
            let mut offsets = vec![FloatPoint::default(); tree.spatial_nodes.len()];
            offsets[sticky_node.0 as usize] = offset;
            list.stack(&tree, &offsets)
        };

        let stack = stack_with_box_held_at(FloatPoint::default());
        assert!(
            stack
                .spaces
                .iter()
                .any(|space| matches!(space.anchor_kind, RasterSpaceAnchorKind::StickyBox { .. }))
        );
        assert!(highest_offset.y > 800.0);
        // The compositor holds the box at offsets of its own as it scrolls the document.
        for y in [800.0, highest_offset.y] {
            assert_eq!(stack_with_box_held_at(FloatPoint { x: 0.0, y }), stack);
        }

        // WebContent does not plan again as it publishes where the box is held.
        let storage = DisplayListStorage::of_builder(&list.builder);
        let tree = Arc::new(tree);
        let inputs_with_box_held_at = |y: f32| {
            let mut offsets = vec![FloatPoint::default(); tree.spatial_nodes.len()];
            offsets[sticky_node.0 as usize].y = y;
            InputsOfLayerPlan::new(storage.clone(), tree.clone(), &offsets, VIEWPORT)
        };
        assert!(inputs_with_box_held_at(0.0).plan_alike_as(&inputs_with_box_held_at(800.0)));
    }

    fn scroller_with_record(
        tree: &mut VisualContextTree,
        records: &mut Vec<CompositorScrollNodeRecord>,
        parent: ContextRef,
        scrollport: IntRect,
        max_scroll_offset_y: f32,
    ) -> ContextRef {
        let clipped_to_scrollport = clipped_context(tree, parent, scrollport, libgfx_rust::CornerRadii::default());
        let scroller = scroll_node(tree, parent.spatial);
        records.push(scroll_record(
            scroller,
            parent.spatial,
            scrollport,
            max_scroll_offset_y,
            false,
        ));
        ContextRef {
            spatial: scroller,
            ..clipped_to_scrollport
        }
    }

    #[test]
    fn a_backdrop_filter_that_the_compositor_scrolls_within_a_space_is_not_placed_under_its_composite_clips() {
        let (mut tree, _, document) = tree_with_document_scroll();
        let mut records = tree.compositor_scroll_table().nodes.to_vec();
        let outer_scroller =
            scroller_with_record(&mut tree, &mut records, document, IntRect::new(0, 0, 400, 400), 10.0);
        for index in 0..5 {
            scroller_with_record(
                &mut tree,
                &mut records,
                document,
                IntRect::new(420, 80 * index, 300, 70),
                500.0,
            );
        }
        // The inner scroller is past the most scrollers that get spaces, so the compositor scrolls it within the space
        // of the outer scroller.
        let inner_scroller = scroller_with_record(
            &mut tree,
            &mut records,
            outer_scroller,
            IntRect::new(0, 100, 400, 200),
            500.0,
        );
        assert!(tree.set_compositor_scroll_table(CompositorScrollTable {
            nodes: records.into(),
            ..CompositorScrollTable::default()
        }));
        let glass = frosted_glass(&mut tree, inner_scroller, IntRect::new(50, 150, 100, 100));
        let planned = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(fill(IntRect::new(0, 0, 400, 1000), GREEN), outer_scroller)
            .with(fill(IntRect::new(50, 150, 100, 100), GREEN), glass)
            .planned(&tree, &[]);
        let space_of_outer_scroller = planned
            .stack
            .spaces
            .iter()
            .position(|space| space.anchor == outer_scroller.spatial)
            .expect("the outer scroller anchors a space");
        assert!(
            planned
                .stack
                .spaces
                .iter()
                .all(|space| space.anchor != inner_scroller.spatial)
        );
        // Scrolled to the end of its range, the inner scroller moves the glass far past the outer scrollport, which
        // would cut what the glass reads where it is composited in the space of the outer scroller.
        let stretch_of_glass = planned
            .placement
            .record_stretches
            .last()
            .expect("the run of the glass is placed");
        let run_group_of_glass = &planned.placement.run_groups[stretch_of_glass.run_group_index.0];
        assert_ne!(run_group_of_glass.raster_space_index.0, space_of_outer_scroller);
    }

    #[test]
    fn content_painted_after_a_sticky_toolbar_covers_a_spinner_in_it() {
        use crate::visual_context::StickyData;
        let (mut tree, _, document) = tree_with_document_scroll();
        let mut sticky_data = StickyData::unconstrained(
            document.spatial,
            None,
            crate::visual_context::scroll_state::NO_SCROLL_STATE_SLOT,
            NodeSlotId::INVALID,
        );
        let toolbar_top = TILE_HEIGHT_IN_DEVICE_PIXELS - 60;
        sticky_data.position_relative_to_scroller = FloatPoint {
            x: 0.0,
            y: toolbar_top as f32,
        };
        sticky_data.border_box_size = libgfx_rust::FloatSize {
            width: 800.0,
            height: 120.0,
        };
        sticky_data.scrollport_size = libgfx_rust::FloatSize {
            width: 800.0,
            height: 600.0,
        };
        sticky_data.containing_block_region = FloatRect::new(0.0, 0.0, 800.0, 2000.0);
        sticky_data.needs_parent_offset_adjustment = true;
        sticky_data.inset_top = Some(0.0);
        let toolbar = ContextRef {
            spatial: tree.append_spatial(SpatialData::Sticky(sticky_data), document.spatial),
            ..document
        };
        // The label and the spinner of the toolbar are in different cells, and the card covers both of them.
        let label = IntRect::new(10, toolbar_top + 5, 100, 20);
        let spinner_box = IntRect::new(650, toolbar_top + 85, 20, 20);
        let card = IntRect::new(0, toolbar_top, 800, 120);
        assert_ne!(
            CellRange::touched_by(label),
            CellRange::touched_by(spinner_box),
            "the label and the spinner are in different cells"
        );
        let spinner = transformed_context(&mut tree, toolbar, spinner_box.to_float());
        tree.set_visual_animations(vec![spinning(spinner.spatial)]);
        let list = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(fill(label, GREEN), toolbar)
            .with(fill(spinner_box, GREEN), spinner)
            .with(fill(card, GREEN), document);
        let planned = list.planned(&tree, &[]);
        assert_eq!(
            space_names(&planned.stack),
            [
                "viewport",
                "document",
                "sticky@s2 in document",
                "animated@s3 in sticky@s2 in document"
            ]
        );
        let storage = DisplayListStorage::of_builder(&list.builder);
        let card_position = storage.span_of_run(3).begin;
        let layers_of_card = planned.layers_of_record(card_position);
        assert_eq!(
            layers_of_card.len(),
            1,
            "the card goes into one layer in every cell: {layers_of_card:?}"
        );
        let band_of_card = planned
            .stack
            .bands
            .iter()
            .find(|band| band.record_spans.iter().any(|span| span.contains(card_position)))
            .expect("a band holds the card");
        let layers_tile_keying_puts_card_into_by_cell: Vec<LayerIndex> = CellRange::touched_by(card)
            .coords()
            .map(|coord| {
                CutsOfCell::of(band_of_card, coord.cell_rect())
                    .layer_of_record(card_position)
                    .0
            })
            .collect();
        assert!(
            layers_tile_keying_puts_card_into_by_cell
                .iter()
                .all(|&layer| layer == layers_of_card[0]),
            "tile keying puts the card into the layer the planner marks in every cell: \
             {layers_tile_keying_puts_card_into_by_cell:?}"
        );
        let layer_of_card = &planned.stack.layers[layers_of_card[0].0];
        assert!(matches!(
            layer_of_card.identity,
            LayerIdentity::Painted {
                above_barrier: Some(_),
                ..
            }
        ));
    }

    #[test]
    fn a_stack_at_a_power_of_two_of_its_raster_scale_cuts_each_tile_as_the_plan_cuts_the_cell_around_it() {
        use crate::display_list::tiles::TileCoord;
        let (tree, _, document) = tree_with_document_scroll();
        let stack = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(canvas(IntRect::new(500, 490, 100, 40), 1), document)
            .with(fill(IntRect::new(400, 400, 400, 400), GREEN), document)
            .stack(&tree, &[]);
        let band_with_cut = stack
            .bands
            .iter()
            .position(|band| !band.cuts.is_empty())
            .expect("the canvas cuts a band");
        let mut scroll_offsets = vec![FloatPoint::default(); tree.spatial_nodes.len()];
        scroll_offsets[document.spatial.0 as usize] = FloatPoint { x: 0.0, y: 100.0 };
        let composite_offsets_of_plan = stack.composite_offsets(&scroll_offsets);
        for factor in [2, 4] {
            let stack_at_scale = stack.at_raster_scale(factor);
            let tiles = CellRange::touched_by(IntRect::new(0, 0, 1024 * 2 * factor, 512 * 3 * factor));
            for coord in tiles.coords() {
                let cell_of_plan =
                    TileCoord::new(coord.column.div_euclid(factor), coord.row.div_euclid(factor)).cell_rect();
                let layers_above_cuts = |stack: &LayerStack, cell: IntRect| -> Vec<LayerIndex> {
                    CutsOfCell::of(&stack.bands[band_with_cut], cell)
                        .cuts()
                        .map(|cut| cut.layer_above_cut)
                        .collect()
                };
                assert_eq!(
                    layers_above_cuts(&stack_at_scale, coord.cell_rect()),
                    layers_above_cuts(&stack, cell_of_plan),
                    "tile {coord} at {factor} raster pixels per device pixel"
                );
            }
            let composite_offsets_at_scale: Vec<libgfx_rust::IntPoint> = composite_offsets_of_plan
                .iter()
                .map(|offset| libgfx_rust::IntPoint {
                    x: offset.x * factor,
                    y: offset.y * factor,
                })
                .collect();
            assert_eq!(
                stack_at_scale.composite_offsets(&scroll_offsets),
                composite_offsets_at_scale
            );
        }
    }

    #[test]
    fn a_slide_farther_than_extents_reach_reaches_the_whole_viewport() {
        let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
        let slider = transformed_context(&mut tree, viewport, FloatRect::new(0.0, 0.0, 50.0, 50.0));
        tree.set_visual_animations(vec![transform_animation_of(
            slider.spatial,
            crate::host::FfiVisualAnimationTransformOperationKind::TranslateX,
            -3.0e9,
            3.0e9,
        )]);
        let stack = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 50, 50), GREEN), slider)
            .with(fill(IntRect::new(0, 50, 100, 100), GREEN), viewport)
            .stack(&tree, &[]);
        assert_eq!(space_names(&stack), ["viewport", "animated@s1 in viewport"]);
        assert_eq!(
            stack.swept_reach_in_parent_space_by_space_index[1],
            Some(IntRect::new(0, 0, 800, 56))
        );
    }

    #[cfg(debug_assertions)]
    #[test]
    #[should_panic(expected = "the compositor refuses a plan of layers the planner made")]
    fn the_planner_runs_the_checks_of_the_compositor_on_a_plan_of_more_layers() {
        let (tree, _, document) = tree_with_document_scroll();
        let viewport_past_coordinate_limit = IntRect::new(0, 0, 600_000_000, 600);
        let list = ListOfOneChunk::default().with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document);
        plan_layer_stack(&LayerStackInputs {
            list: &DisplayListStorage::of_builder(&list.builder),
            tree: &tree,
            scroll_offsets: &[],
            viewport_rect: viewport_past_coordinate_limit,
        });
    }

    #[test]
    fn a_plan_holds_for_a_tree_that_differs_only_in_what_the_main_thread_keeps_of_its_nodes() {
        let (tree, _, document) = tree_with_document_scroll();
        let list = ListOfOneChunk::default().with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document);
        let storage = DisplayListStorage::of_builder(&list.builder);
        let mut tree_of_other_paintable = tree.clone();
        let SpatialData::Scroll(scroll) = &mut tree_of_other_paintable.spatial_nodes[document.spatial.0 as usize].data
        else {
            unreachable!("the document scrolls");
        };
        scroll.state_slot = 3;
        scroll.owner_paintable = NodeSlotId::new(5, 1);
        let inputs = InputsOfLayerPlan::new(storage.clone(), Arc::new(tree), &[], VIEWPORT);
        assert!(inputs.plan_alike_as(&InputsOfLayerPlan::new(
            storage,
            Arc::new(tree_of_other_paintable),
            &[],
            VIEWPORT
        )));
    }

    #[test]
    fn a_plan_holds_for_inputs_that_differ_only_in_what_it_does_not_read() {
        let (mut tree, _, document) = tree_with_document_scroll();
        let scroller = scroll_node(&mut tree, document.spatial);
        let list = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(
                fill(IntRect::new(0, 0, 100, 100), GREEN),
                ContextRef {
                    spatial: scroller,
                    ..document
                },
            );
        let storage = DisplayListStorage::of_builder(&list.builder);
        let tree = Arc::new(tree);
        let offsets = |document_y: f32, scroller_y: f32| {
            let mut offsets = vec![FloatPoint::default(); tree.spatial_nodes.len()];
            offsets[document.spatial.0 as usize].y = document_y;
            offsets[scroller.0 as usize].y = scroller_y;
            offsets
        };
        let inputs = InputsOfLayerPlan::new(storage.clone(), tree.clone(), &offsets(0.0, 0.0), VIEWPORT);

        // The compositor scrolls the document, which has a row in its scroll table, and not the scroller.
        assert!(inputs.plan_alike_as(&inputs.with_scroll_offsets_and_viewport_rect(&offsets(700.0, 0.0), VIEWPORT)));
        // Inputs made after these for the same tree take what plans work out of the tree alone from them.
        let inputs_after = InputsOfLayerPlan::new_after(
            Some(&inputs),
            storage.clone(),
            tree.clone(),
            &offsets(700.0, 0.0),
            VIEWPORT,
        );
        assert!(Arc::ptr_eq(&inputs.anchors_of_tree, &inputs_after.anchors_of_tree));
        assert!(inputs.plan_alike_as(&inputs_after));
        assert!(!inputs.plan_alike_as(&inputs.with_scroll_offsets_and_viewport_rect(&offsets(0.0, 30.0), VIEWPORT)));
        assert!(!inputs.plan_alike_as(
            &inputs.with_scroll_offsets_and_viewport_rect(&offsets(0.0, 0.0), IntRect::new(0, 0, 800, 500))
        ));
        // A list is told apart by its chunks, and a tree by its content.
        let list_of_same_chunks = DisplayListStorage::of_builder(&list.builder);
        let tree_of_same_content = Arc::new((*tree).clone());
        assert!(inputs.plan_alike_as(&InputsOfLayerPlan::new(
            list_of_same_chunks,
            tree_of_same_content,
            &[],
            VIEWPORT
        )));
        let mut tree_of_other_content = (*tree).clone();
        scroll_node(&mut tree_of_other_content, document.spatial);
        assert!(!inputs.plan_alike_as(&InputsOfLayerPlan::new(
            storage,
            Arc::new(tree_of_other_content),
            &[],
            VIEWPORT
        )));
        let other_list = ListOfOneChunk::default().with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document);
        assert!(!inputs.plan_alike_as(&InputsOfLayerPlan::new(
            DisplayListStorage::of_builder(&other_list.builder),
            tree.clone(),
            &[],
            VIEWPORT
        )));
        assert_eq!(inputs.plan().stack, list.stack(&tree, &offsets(700.0, 0.0)));
    }
}
