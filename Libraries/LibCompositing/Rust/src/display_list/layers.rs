/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The layers a display list is painted in, bottom to top: the stack, what the compositor and tile keying read of it,
//! and its wire form.
//!
//! Content is rastered in the space of the scroll node, sticky box or node whose transform the compositor animates
//! that it moves with, and its raster is composited within the clips it leaves to apply then (its composite clips), and
//! inside the effects the compositor applies to it: content of one space, one set of composite clips and at most one
//! composited effect makes a placement node. The records of a node are cut into bands. Within a band, live content that
//! is a layer of its own (a canvas, video, nested context, caret or scroll bar), and each band of a node nested in it,
//! sits between the band's records as a barrier. A barrier cuts the band's tiles that it can show in, or for an effect
//! that reads what is composited below it, that it reads, at its paint position: there, the band's records painted
//! before the barrier go into the layer below the cut and those painted after it into the layer above the cut.
//! Elsewhere it cuts nothing. Live content cuts no tile where no record of its band painted after it can show where it
//! can.

use crate::display_list::commands::ClipNodeIndex;
use crate::display_list::commands::{EffectNodeIndex, SpatialNodeIndex};
use crate::display_list::effect_geometry::{
    LARGEST_FILTER_READ_REACH_IN_DEVICE_PIXELS, filter_may_spread_at_some_sample,
};
pub use crate::display_list::placement::{
    CompositeClip, RasterSpace, RasterSpaceAnchorKind, RasterSpaceIndex, RunGroupIndex, raster_space_anchors,
};
use crate::display_list::placement::{
    EXTENT_LIMIT_IN_DEVICE_PIXELS, enclosing_int_rect_within_extent_limit, space_and_ancestors_below_viewport,
    spatial_nodes_that_move_on_their_own,
};
use crate::display_list::reach::Reach;
use crate::display_list::storage::{DisplayListStorage, ListPosition, ListSpan};
use crate::display_list::summary::LiveContent;
use crate::display_list::window_server_placement::{
    WhyWindowServerCannotCompositeEffect, WhyWindowServerCannotShowMedia, WindowServerPlacement,
    WindowServerPlacementOfEffect, window_server_placement_of_effect,
};
use crate::visual_context::queries::backdrop_reader_by_reading_effect_index;
use crate::visual_context::{VisualContextTree, WorldTransforms, device_offset_for_index};
use libgfx_rust::{AffineTransform, CornerRadii, FloatMatrix4x4, FloatPoint, IntPoint, IntRect};
use std::hash::{BuildHasher, Hash, Hasher};
use std::rc::Rc;

pub mod planning;
mod wire;

pub use wire::{LayerPlan, UnvalidatedLayerPlan, compositor_takes_plans_within};

// Twice the extent limit of a plan, which leaves room for the margins that planning adds past the extent of a space.
// The rects of a stack from another process are within it, and so are the offsets a frame composites spaces at, so that
// a rect offset by such an offset still fits in an `IntRect`.
const COORDINATE_LIMIT_IN_DEVICE_PIXELS: i32 = 2 * EXTENT_LIMIT_IN_DEVICE_PIXELS as i32;

/// The most carets and scroll bars that get layers, each of which the compositor paints into a small surface of its
/// own. Each cuts its band once, and a plan cuts a band at most `MAXIMUM_CUT_COUNT_OF_RECEIVED_BAND` times. Canvases,
/// videos and nested contexts are bounded only by those cuts.
pub const MAXIMUM_LAYER_COUNT_OF_LIVE_CONTENT_DRAWN_BY_ITS_RECORD: usize = 64;

/// The most composited effects that nest around a layer of a plan: the effects a frame composites the layer inside,
/// each inside the next. Pages nest far fewer, as the compositor composites only an effect that it animates, or one
/// around a layer of its own, such as of a scroller, a sticky box or a canvas.
pub const MAXIMUM_NESTING_OF_COMPOSITED_EFFECTS: usize = 32;

/// The farthest that the filters of the effects a frame composites around a layer read past each pixel of it, summed, in
/// device pixels. A frame keys the tiles of a layer that far past what it shows, and a larger sum is cut to this.
pub const LARGEST_SUMMED_READ_REACH_OF_COMPOSITED_FILTERS_IN_DEVICE_PIXELS: i32 =
    4 * LARGEST_FILTER_READ_REACH_IN_DEVICE_PIXELS as i32;

pub(crate) fn media_name(content: LiveContent) -> &'static str {
    match content {
        LiveContent::Canvas(_) => "canvas",
        LiveContent::VideoFrame(_) => "video",
        LiveContent::ImageFrame(_) => "image",
        LiveContent::CompositedContext(_) => "composited context",
        LiveContent::Caret => "caret",
        LiveContent::ScrollBar { vertical: true, .. } => "vertical scroll bar",
        LiveContent::ScrollBar { vertical: false, .. } => "horizontal scroll bar",
    }
}

/// Why the compositor cannot composite an effect, which keeps what it holds from having layers of its own.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WhyCompositorCannotCompositeEffect {
    FilterMovesWhatItFilters,
    FilterReadsFartherThanTilesAreFollowed,
}

impl std::fmt::Display for WhyCompositorCannotCompositeEffect {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(match self {
            WhyCompositorCannotCompositeEffect::FilterMovesWhatItFilters => "filter that moves what it filters",
            WhyCompositorCannotCompositeEffect::FilterReadsFartherThanTilesAreFollowed => {
                "filter that reads farther than tiles are followed"
            }
        })
    }
}

/// Why a list is painted in a single layer that replays all of it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WhyListIsOneLayer {
    EmptyViewport,
    EmptyDisplayList,
    SortingContext3D,
    RootIsolationEffectIsNotPlainGroup,
    RecordsBlendWithWhatOtherLayersHoldBelowThem,
    EffectTheCompositorCannotCompositeIsComposited(WhyCompositorCannotCompositeEffect),
    CompositedEffectsNestTooDeep,
    PlanHoldsMoreThanCompositorDecodes,
    /// The frame draws the list otherwise than a plan of its layers is made for, as through a raster transform or a
    /// visual viewport transform the plan does not know.
    FrameIsNotDrawnAsPlanned,
    /// A frame drawn as the plan was made for shows more tiles than the compositor holds for a frame.
    PlanShowsTooManyTiles,
    /// The plan the planner made fails the checks the compositor runs on a plan it receives.
    PlanFailsChecksOfCompositor,
}

impl std::fmt::Display for WhyListIsOneLayer {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(match self {
            WhyListIsOneLayer::EmptyViewport => "empty viewport",
            WhyListIsOneLayer::EmptyDisplayList => "empty display list",
            WhyListIsOneLayer::SortingContext3D => "3D sorting context holding an effect that reads or is composited",
            WhyListIsOneLayer::RootIsolationEffectIsNotPlainGroup => "root isolation effect is not a plain group",
            WhyListIsOneLayer::RecordsBlendWithWhatOtherLayersHoldBelowThem => {
                "records blend with what other layers hold below them"
            }
            WhyListIsOneLayer::EffectTheCompositorCannotCompositeIsComposited(why) => {
                return write!(formatter, "{why} is composited");
            }
            WhyListIsOneLayer::CompositedEffectsNestTooDeep => {
                return write!(
                    formatter,
                    "composited effects nest deeper than {MAXIMUM_NESTING_OF_COMPOSITED_EFFECTS}"
                );
            }
            WhyListIsOneLayer::PlanHoldsMoreThanCompositorDecodes => {
                "plan holds more nodes, bands, cuts or spans than the compositor decodes"
            }
            WhyListIsOneLayer::FrameIsNotDrawnAsPlanned => "frame is not drawn as the plan was made for",
            WhyListIsOneLayer::PlanShowsTooManyTiles => "plan shows more tiles than a frame holds",
            WhyListIsOneLayer::PlanFailsChecksOfCompositor => "plan fails the checks of the compositor",
        })
    }
}

/// How the lines that describe a stack name the visual context nodes they mention.
pub trait VisualContextNodeNamer {
    fn name_of_spatial_node(&self, index: SpatialNodeIndex) -> String;
    fn name_of_effect_node(&self, index: EffectNodeIndex) -> String;
}

/// Names visual context nodes by their indices in the tree.
pub struct NodeNamesFromIndices;

impl VisualContextNodeNamer for NodeNamesFromIndices {
    fn name_of_spatial_node(&self, index: SpatialNodeIndex) -> String {
        format!("s{}", index.0)
    }

    fn name_of_effect_node(&self, index: EffectNodeIndex) -> String {
        format!("e{}", index.0)
    }
}

/// Names a placement node by the identity of its space, the clips its rasters are composited within, and the effects
/// they are composited inside, which outlive the tree and the list.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct PlacementNodeIdentity(pub u64);

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum BarrierIdentity {
    /// The `ordinal`th drawing of live content in paint order.
    Media { content: LiveContent, ordinal: u32 },
    /// The `band_ordinal_in_node`th band of content of a node, counted over the whole list.
    NestedNodeBand {
        node: PlacementNodeIdentity,
        band_ordinal_in_node: u32,
    },
}

/// Names a layer from one list to the next, so that its tiles outlive both.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum LayerIdentity {
    Painted {
        node: PlacementNodeIdentity,
        band_ordinal_in_node: u32,
        above_barrier: Option<BarrierIdentity>,
    },
    Media {
        content: LiveContent,
        ordinal: u32,
    },
}

/// How the frame composites the rasters of a space.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum SpaceToFrame {
    /// Translated by whole device pixels.
    Offset(IntPoint),
    /// Through a transform from the space's raster pixels to the frame's, for a space whose anchor the compositor
    /// animates.
    Transform(AffineTransform),
}

impl Eq for SpaceToFrame {}

// A rect that a transform maps past the extent limit shows nothing there, and is kept within the limit so that the
// arithmetic of the rect does not overflow.
impl SpaceToFrame {
    /// The frame's pixels that `rect_in_space` covers, or the bounds of them.
    pub fn rect_in_frame(self, rect_in_space: IntRect) -> IntRect {
        match self {
            SpaceToFrame::Offset(offset) => rect_in_space.translated(offset.x, offset.y),
            SpaceToFrame::Transform(_) if rect_in_space.is_empty() => IntRect::default(),
            SpaceToFrame::Transform(transform) => transform_and_inverse_that_show_space(transform)
                .map_or(IntRect::default(), |(transform, _)| {
                    enclosing_int_rect_within_extent_limit(transform.map_rect(rect_in_space.to_float()))
                }),
        }
    }

    /// The space's pixels that `rect_in_frame` covers, or the bounds of them.
    pub fn rect_in_space(self, rect_in_frame: IntRect) -> IntRect {
        match self {
            SpaceToFrame::Offset(offset) => rect_in_frame.translated(-offset.x, -offset.y),
            SpaceToFrame::Transform(_) if rect_in_frame.is_empty() => IntRect::default(),
            SpaceToFrame::Transform(transform) => transform_and_inverse_that_show_space(transform)
                .map_or(IntRect::default(), |(_, inverse)| {
                    enclosing_int_rect_within_extent_limit(inverse.map_rect(rect_in_frame.to_float()))
                }),
        }
    }
}

/// A transform of a space and its inverse, or none for a transform that shows nothing of the space: one with a value
/// that is not finite, or one without an inverse of finite values.
fn transform_and_inverse_that_show_space(transform: AffineTransform) -> Option<(AffineTransform, AffineTransform)> {
    let is_finite = |transform: AffineTransform| transform.values.iter().all(|value| value.is_finite());
    let inverse = transform.inverse()?;
    (is_finite(transform) && is_finite(inverse)).then_some((transform, inverse))
}

/// The index of a node in `LayerStack::nodes`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct PlacementNodeIndex(pub usize);

/// The index of a band in `LayerStack::bands`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct BandIndex(pub usize);

/// The index of a layer in `LayerStack::layers`, bottom to top.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct LayerIndex(pub usize);

/// Content rastered in one space, composited within one set of clips, and inside at most one effect that the
/// compositor applies, which a replay of the list would apply around it.
#[derive(Clone, Debug, PartialEq)]
pub struct PlacementNode {
    pub identity: PlacementNodeIdentity,
    pub raster_space_index: RasterSpaceIndex,
    /// At most one per space, ordered by space.
    pub composite_clips: Vec<CompositeClip>,
    /// The effect the compositor composites the rasters of the node inside, within those it composites the parent
    /// node inside. The rasters leave it out, with the effects around it and the clips it is entered within.
    pub composited_effect: Option<EffectNodeIndex>,
    pub parent_index: Option<PlacementNodeIndex>,
    /// Where content of the node and of the nodes in it can show, or a composited effect read.
    pub reach_in_parent_node: Reach<IntRect>,
}

/// Where a barrier cuts the tiles of a band.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct BarrierCut {
    pub barrier_first_record_position: ListPosition,
    /// The barrier cuts the tiles of the band's space this reaches.
    pub cut_reach: Reach<IntRect>,
    pub layer_above_cut: LayerIndex,
}

impl BarrierCut {
    pub fn cuts_cell(&self, cell: IntRect) -> bool {
        self.cut_reach.touches(cell)
    }
}

/// One band of a placement node: the records placed in the node while the band lasts, and the barriers that cut them.
#[derive(Clone, Debug, PartialEq)]
pub struct PlacementNodeBand {
    pub placement_node_index: PlacementNodeIndex,
    pub band_ordinal_in_node: u32,
    pub parent_band_holding_this_band_as_barrier: Option<BandIndex>,
    /// In paint order.
    pub record_spans: Vec<ListSpan>,
    /// In paint order.
    pub cuts: Vec<BarrierCut>,
    pub layer_below_all_cuts: LayerIndex,
}

/// The barriers of a band that cut the tile at one cell, in paint order, worked out once for the cell. A band of a
/// received stack can have too many barriers to look through for each record of the cell.
pub struct CutsOfCell<'a> {
    band: &'a PlacementNodeBand,
    cut_indices: Vec<u32>,
}

impl<'a> CutsOfCell<'a> {
    pub fn of(band: &'a PlacementNodeBand, cell: IntRect) -> Self {
        Self {
            band,
            cut_indices: (0..band.cuts.len() as u32)
                .filter(|&cut_index| band.cuts[cut_index as usize].cuts_cell(cell))
                .collect(),
        }
    }

    /// The barriers that cut the tile, in paint order.
    pub fn cuts(&self) -> impl Iterator<Item = &'a BarrierCut> + '_ {
        let band = self.band;
        self.cut_indices
            .iter()
            .map(move |&cut_index| &band.cuts[cut_index as usize])
    }

    /// The layer the band's record at `record_position` goes into within the tile: the one above the last barrier
    /// painted before it that cuts the tile. Answers too how many of the barriers that cut the tile are below it.
    pub fn layer_of_record(&self, record_position: ListPosition) -> (LayerIndex, usize) {
        // The cuts of a band are in paint order.
        let count_of_cuts_below = self.cut_indices.partition_point(|&cut_index| {
            self.band.cuts[cut_index as usize].barrier_first_record_position <= record_position
        });
        let layer = match count_of_cuts_below {
            0 => self.band.layer_below_all_cuts,
            count => self.band.cuts[self.cut_indices[count - 1] as usize].layer_above_cut,
        };
        (layer, count_of_cuts_below)
    }

    /// Which barriers cut the tile: tiles that answer alike place every record alike.
    pub fn barrier_cuts(&self) -> BarrierCutsOfCell {
        if self.band.cuts.len() <= u64::BITS as usize {
            BarrierCutsOfCell::BitPerCut(
                self.cut_indices
                    .iter()
                    .fold(0, |bits, &cut_index| bits | (1u64 << cut_index)),
            )
        } else {
            BarrierCutsOfCell::CutIndices(self.cut_indices.as_slice().into())
        }
    }
}

/// Which barriers of a band cut a tile.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum BarrierCutsOfCell {
    /// A bit per barrier, in paint order, of a band with no more barriers than bits.
    BitPerCut(u64),
    /// The indices of the barriers, in paint order, of a band with more.
    CutIndices(Rc<[u32]>),
}

#[derive(Clone, Debug, PartialEq)]
pub enum LayerContent {
    /// The records of a band that the layer holds in some of its tiles.
    Painted,
    /// A canvas, video or nested context, replayed or shown by the window server, whose content the identity of the
    /// layer names.
    Media { record_span: ListSpan },
    /// A caret or a scroll bar, alone in the run of `record_span`, which a frame replays and a window server that shows
    /// tiles shows from a surface of its own.
    OwnSurface { record_span: ListSpan },
}

#[derive(Clone, Debug, PartialEq)]
pub struct StackLayer {
    pub identity: LayerIdentity,
    pub placement_node_index: PlacementNodeIndex,
    /// The band the layer holds the records of, or the one it is a barrier in.
    pub band_index: BandIndex,
    pub content: LayerContent,
    /// Whether any record can go into the layer anywhere; a layer that holds nothing has no tiles.
    pub may_hold_content: bool,
    /// Where what the layer holds can show, in the raster coordinates of its space.
    pub reach: IntRect,
}

/// An effect that the compositor composites the layers of its content inside.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CompositedEffect {
    pub effect: EffectNodeIndex,
    pub placement_node_index: PlacementNodeIndex,
    /// Whether the effect reads what is composited below it: it reads the backdrop of a backdrop filter, or it blends.
    pub reads_what_is_composited_below_it: bool,
    /// Whether its filter moves what it composites past where it is drawn, so that it reads what is drawn inside it
    /// past each of its pixels.
    pub filter_spreads_what_it_composites: bool,
}

impl CompositedEffect {
    /// The effects the compositor composites the nodes of a stack inside, one per composited node in the order of the
    /// nodes, with what the tree tells of how each reads. Each of those effects is a node of `tree`.
    fn of_nodes(nodes: &[PlacementNode], tree: &VisualContextTree) -> Vec<CompositedEffect> {
        if nodes.iter().all(|node| node.composited_effect.is_none()) {
            return Vec::new();
        }
        let backdrop_reader_by_effect_index = backdrop_reader_by_reading_effect_index(tree);
        nodes
            .iter()
            .enumerate()
            .filter_map(|(index, node)| {
                let effect = node.composited_effect?;
                Some(CompositedEffect {
                    effect,
                    placement_node_index: PlacementNodeIndex(index),
                    reads_what_is_composited_below_it: backdrop_reader_by_effect_index[effect.0 as usize].is_some(),
                    filter_spreads_what_it_composites: filter_may_spread_at_some_sample(tree, effect),
                })
            })
            .collect()
    }
}

/// The layers of a list bottom to top, with the base layer, which is rastered over the surface's clear color and
/// replaces what is below it. Layers below the base layer hold nothing. A stack does not change once it is planned:
/// what a frame works out of it is kept apart from it.
#[derive(Clone, Debug, PartialEq)]
pub struct LayerStack {
    /// The viewport's space, and for a stack of more than one layer, the spaces `RasterSpaces::spaces_at_anchors` makes
    /// at the anchors `raster_space_anchors` gives for the tree, without the extent of each space but the viewport's.
    pub spaces: Vec<RasterSpace>,
    pub nodes: Vec<PlacementNode>,
    pub bands: Vec<PlacementNodeBand>,
    pub layers: Vec<StackLayer>,
    /// In the order of their nodes, which is paint order.
    pub composited_effects: Vec<CompositedEffect>,
    pub base_layer: LayerIndex,
    /// Why the stack is one layer that replays the whole list, if it is.
    pub why_list_is_one_layer: Option<WhyListIsOneLayer>,
    pub layering_obstacles: LayeringObstacles,
    /// Per raster space, whether records its tiles hold move within it as nodes that move on their own, or that the
    /// compositor scrolls without the space following them, do. Canvases, videos and nested contexts with layers of
    /// their own hold none of those records.
    pub tiles_hold_content_moving_within_space_by_space_index: Vec<bool>,
    /// Per raster space whose anchor the compositor animates and that holds content, where that content can show in
    /// the raster coordinates of the space it is in wherever the animation takes it.
    pub swept_reach_in_parent_space_by_space_index: Vec<Option<IntRect>>,
    /// Per band, the innermost band at or around it whose node the compositor composites inside its effect.
    innermost_composited_band_at_or_around_band: Vec<Option<BandIndex>>,
}

/// What a window server composites by itself of the layers of a frame, besides the canvases and videos it shows.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct WindowServerCompositing {
    /// Whether it shows each tile of a painted layer as a layer of its own, which it moves with the layer's space,
    /// through the transform of a space the compositor animates too, clips to the layer's composite clips, and
    /// composites inside the effects it can composite, so that a frame all of whose layers it shows needs no surface
    /// of its own.
    pub shows_tiles_of_painted_layers: bool,
    /// Whether it runs what it shows below a layer through a blur and color matrices, as a backdrop filter does.
    pub filters_what_is_below_layers: bool,
}

/// What the window server can show by itself of a stack under the values of one frame.
pub struct WhatWindowServerShowsOfFrame<'a> {
    pub compositing: WindowServerCompositing,
    /// Whether it can show the canvas, video or nested context of a media layer by itself.
    pub can_show_media_of_layer: &'a dyn Fn(LayerIndex) -> bool,
    /// Whether it can composite the effect of a composited node by itself, around the layers the node holds.
    pub can_composite_effect_of_node: &'a dyn Fn(PlacementNodeIndex) -> bool,
}

/// How the window server can composite each composited effect of a stack under the values of one frame, or why it
/// cannot, by the index of the effect's node.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct WindowServerPlacementsOfEffects {
    placement_by_node_index: Vec<Option<Result<WindowServerPlacementOfEffect, WhyWindowServerCannotCompositeEffect>>>,
}

impl WindowServerPlacementsOfEffects {
    /// How the window server can composite the effect of the node, or why it cannot. None for a node that is
    /// composited inside no effect of its own.
    pub fn of_node(
        &self,
        node_index: PlacementNodeIndex,
    ) -> Option<&Result<WindowServerPlacementOfEffect, WhyWindowServerCannotCompositeEffect>> {
        self.placement_by_node_index.get(node_index.0)?.as_ref()
    }
}

/// Why a frame holds a layer, compositing it itself, rather than the window server showing the layer by itself.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WhyFrameHoldsLayer {
    ListIsOneLayer,
    InsideAnEffectTheCompositorComposites,
    CompositedWithinRoundedCorners,
    InSpaceTheCompositorAnimates,
    WindowServerCannotShowItsContent,
    UnderALayerTheFrameHolds,
    WindowServerShowsNoTiles,
}

impl std::fmt::Display for WhyFrameHoldsLayer {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(match self {
            WhyFrameHoldsLayer::ListIsOneLayer => "the list is one layer",
            WhyFrameHoldsLayer::InsideAnEffectTheCompositorComposites => {
                "it is inside an effect the compositor composites"
            }
            WhyFrameHoldsLayer::CompositedWithinRoundedCorners => "it is composited within rounded corners",
            WhyFrameHoldsLayer::InSpaceTheCompositorAnimates => "it is in a space the compositor animates",
            WhyFrameHoldsLayer::WindowServerCannotShowItsContent => "the window server cannot show its content",
            WhyFrameHoldsLayer::UnderALayerTheFrameHolds => "it is under a layer the frame holds",
            WhyFrameHoldsLayer::WindowServerShowsNoTiles => "the window server shows no tiles",
        })
    }
}

/// Where the window server can show each canvas and video layer of a stack under the values of one frame, or why it
/// cannot, by layer index. A stack is kept while its spaces scroll and its media move with them, so each frame works
/// this out anew.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct WindowServerPlacementsOfMedia {
    placement_by_layer_index: Vec<Option<Result<WindowServerPlacement, WhyWindowServerCannotShowMedia>>>,
}

impl WindowServerPlacementsOfMedia {
    /// Where the window server can show the canvas or video of the layer, or why it cannot. None for a layer that holds
    /// no canvas or video.
    pub fn of_layer(
        &self,
        layer_index: LayerIndex,
    ) -> Option<&Result<WindowServerPlacement, WhyWindowServerCannotShowMedia>> {
        self.placement_by_layer_index.get(layer_index.0)?.as_ref()
    }
}

/// Where the window server can show the canvases and videos of a stack under the values of one frame, and which layers
/// it shows by itself where it can show every canvas and video it has a placement for, as the dump tells them.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct WindowServerPresentationOfMedia {
    pub placements: WindowServerPlacementsOfMedia,
    pub placements_of_effects: WindowServerPlacementsOfEffects,
    /// By layer index.
    pub shown_by_window_server_by_layer_index: Vec<bool>,
    pub window_server_compositing: WindowServerCompositing,
}

impl WindowServerPresentationOfMedia {
    fn layer_is_shown_by_window_server(&self, layer_index: LayerIndex) -> bool {
        self.shown_by_window_server_by_layer_index
            .get(layer_index.0)
            .copied()
            .unwrap_or(false)
    }

    /// Why the frame holds a painted layer of `stack`, which the presentation is of, where the window server shows
    /// tiles and still does not show the layer by itself.
    fn why_frame_holds_painted_layer(&self, stack: &LayerStack, layer_index: LayerIndex) -> Option<WhyFrameHoldsLayer> {
        if !self.window_server_compositing.shows_tiles_of_painted_layers
            || self.layer_is_shown_by_window_server(layer_index)
        {
            return None;
        }
        Some(self.with_what_window_server_shows_of_frame(|window_server| {
            stack.why_frame_holds_layer(layer_index, window_server)
        }))
    }

    /// Hands `read` what the window server shows by itself where it can show every canvas and video it has a
    /// placement for, and composite every effect it has a placement for.
    fn with_what_window_server_shows_of_frame<Answer>(
        &self,
        read: impl FnOnce(&WhatWindowServerShowsOfFrame<'_>) -> Answer,
    ) -> Answer {
        read(&WhatWindowServerShowsOfFrame {
            compositing: self.window_server_compositing,
            can_show_media_of_layer: &|layer_index| matches!(self.placements.of_layer(layer_index), Some(Ok(_))),
            can_composite_effect_of_node: &|node_index| {
                matches!(self.placements_of_effects.of_node(node_index), Some(Ok(_)))
            },
        })
    }
}

// A band comes after the band it is in.
fn innermost_composited_band_at_or_around_each_band(
    nodes: &[PlacementNode],
    bands: &[PlacementNodeBand],
) -> Vec<Option<BandIndex>> {
    let mut innermost_composited_band_by_band_index: Vec<Option<BandIndex>> = Vec::with_capacity(bands.len());
    for (band_index, band) in bands.iter().enumerate() {
        let innermost_composited_band = if nodes[band.placement_node_index.0].composited_effect.is_some() {
            Some(BandIndex(band_index))
        } else {
            band.parent_band_holding_this_band_as_barrier
                .and_then(|band_around| innermost_composited_band_by_band_index.get(band_around.0).copied())
                .flatten()
        };
        innermost_composited_band_by_band_index.push(innermost_composited_band);
    }
    innermost_composited_band_by_band_index
}

fn identity_of_node(
    spaces: &[RasterSpace],
    raster_space_index: RasterSpaceIndex,
    composite_clips: &[CompositeClip],
) -> PlacementNodeIdentity {
    let mut hasher = foldhash::fast::FixedState::with_seed(0x706c_6163_656d_656e).build_hasher();
    hasher.write_u64(spaces[raster_space_index.0].identity);
    for clip in composite_clips {
        hasher.write_u64(spaces[clip.raster_space_index.0].identity);
        for value in [clip.rect.x, clip.rect.y, clip.rect.width, clip.rect.height] {
            hasher.write_i32(value);
        }
        clip.corner_radii.hash(&mut hasher);
    }
    PlacementNodeIdentity(hasher.finish())
}

// An effect index names the effect for as long as the tree keeps the node, which outlives lists and scrolls.
fn identity_of_composited_node(
    parent_identity: PlacementNodeIdentity,
    effect: EffectNodeIndex,
) -> PlacementNodeIdentity {
    let mut hasher = foldhash::fast::FixedState::with_seed(0x636f_6d70_6f73_6974).build_hasher();
    hasher.write_u64(parent_identity.0);
    hasher.write_u32(effect.0);
    PlacementNodeIdentity(hasher.finish())
}

// A node of a space inside a composited node is named apart from the node of the same space and clips outside it.
fn identity_of_node_inside_composited_node(
    composited_node_identity: PlacementNodeIdentity,
    identity_outside_composited_nodes: PlacementNodeIdentity,
) -> PlacementNodeIdentity {
    let mut hasher = foldhash::fast::FixedState::with_seed(0x696e_7369_6465_636e).build_hasher();
    hasher.write_u64(composited_node_identity.0);
    hasher.write_u64(identity_outside_composited_nodes.0);
    PlacementNodeIdentity(hasher.finish())
}

fn node_covers_viewport(nodes: &[PlacementNode], spaces: &[RasterSpace], node_index: PlacementNodeIndex) -> bool {
    let mut node_on_way_out = Some(node_index);
    while let Some(index) = node_on_way_out {
        let node = &nodes[index.0];
        if !node.composite_clips.is_empty()
            || node.composited_effect.is_some()
            || node.reach_in_parent_node != Reach::Anywhere
            || (node.parent_index.is_some()
                && spaces[node.raster_space_index.0].anchor_kind != RasterSpaceAnchorKind::ViewportScroller)
        {
            return false;
        }
        node_on_way_out = node.parent_index;
    }
    true
}

/// What a plan says of a stack of more than one layer, each thing once: the nodes without their identities, and the
/// bands in the order of a walk that enters the first band, and at each cut whose barrier is a nested band, the next
/// band, each band with what only it knows of its layers. `LayerStack::of_layered_plan` works out the rest.
#[derive(Clone, Debug)]
struct LayeredPlan {
    nodes: Vec<NodeOfPlan>,
    bands: Vec<BandOfPlan>,
    tiles_hold_content_moving_within_space_by_space_index: Vec<bool>,
    swept_reach_in_parent_space_by_space_index: Vec<Option<IntRect>>,
}

/// A placement node without its identity, which the stack works out of the node, the node it is in and the spaces.
#[derive(Clone, Debug)]
struct NodeOfPlan {
    raster_space_index: RasterSpaceIndex,
    composite_clips: Vec<CompositeClip>,
    composited_effect: Option<EffectNodeIndex>,
    parent_index: Option<PlacementNodeIndex>,
    reach_in_parent_node: Reach<IntRect>,
}

#[derive(Clone, Debug)]
struct BandOfPlan {
    placement_node_index: PlacementNodeIndex,
    /// In paint order.
    record_spans: Vec<ListSpan>,
    reach_of_layer_below_all_cuts: IntRect,
    /// In paint order.
    cuts: Vec<CutOfPlan>,
}

#[derive(Clone, Copy, Debug)]
struct CutOfPlan {
    barrier: BarrierOfPlan,
    reach_of_layer_above_cut: IntRect,
}

#[derive(Clone, Copy, Debug)]
enum BarrierOfPlan {
    /// Live content that is a layer of its own, the `ordinal`th drawing of its content in paint order. Where `cuts_band`
    /// is set, it cuts the tiles its reach touches, and otherwise no tile, as no record of the band painted after it can
    /// show where it can.
    Media {
        content: LiveContent,
        ordinal: u32,
        record_span: ListSpan,
        reach: IntRect,
        cuts_band: bool,
    },
    /// The next band of the walk, which is of a node in the band's node and cuts the tiles that node reaches.
    NestedBand,
}

// A band the walk of the bands of a plan is in, with the cuts of it that the walk has yet to pass, and the reach of the
// layer above the cut whose nested band the walk is in.
struct BandOfWalk {
    band_index: BandIndex,
    cuts_to_pass: std::vec::IntoIter<CutOfPlan>,
    reach_of_layer_above_nested_band: IntRect,
}

fn painted_layer_of_band(
    nodes: &[PlacementNode],
    bands: &[PlacementNodeBand],
    band_index: BandIndex,
    above_barrier: Option<BarrierIdentity>,
    reach: IntRect,
) -> StackLayer {
    let band = &bands[band_index.0];
    StackLayer {
        identity: LayerIdentity::Painted {
            node: nodes[band.placement_node_index.0].identity,
            band_ordinal_in_node: band.band_ordinal_in_node,
            above_barrier,
        },
        placement_node_index: band.placement_node_index,
        band_index,
        content: LayerContent::Painted,
        may_hold_content: !reach.is_empty(),
        reach,
    }
}

impl LayerStack {
    /// One layer that replays the whole list, which is always correct.
    pub fn one_layer_replaying_whole_list(
        viewport_rect: IntRect,
        whole_list_span: ListSpan,
        why_list_is_one_layer: Option<WhyListIsOneLayer>,
        layering_obstacles: LayeringObstacles,
    ) -> Self {
        let spaces = vec![RasterSpace::of_viewport(viewport_rect)];
        let root_node = PlacementNode {
            identity: identity_of_node(&spaces, RasterSpaceIndex::VIEWPORT, &[]),
            raster_space_index: RasterSpaceIndex::VIEWPORT,
            composite_clips: Vec::new(),
            composited_effect: None,
            parent_index: None,
            reach_in_parent_node: Reach::Anywhere,
        };
        let layers = vec![StackLayer {
            identity: LayerIdentity::Painted {
                node: root_node.identity,
                band_ordinal_in_node: 0,
                above_barrier: None,
            },
            placement_node_index: PlacementNodeIndex(0),
            band_index: BandIndex(0),
            content: LayerContent::Painted,
            may_hold_content: true,
            reach: viewport_rect,
        }];
        Self {
            layers,
            bands: vec![PlacementNodeBand {
                placement_node_index: PlacementNodeIndex(0),
                band_ordinal_in_node: 0,
                parent_band_holding_this_band_as_barrier: None,
                record_spans: vec![whole_list_span],
                cuts: Vec::new(),
                layer_below_all_cuts: LayerIndex(0),
            }],
            nodes: vec![root_node],
            spaces,
            composited_effects: Vec::new(),
            base_layer: LayerIndex(0),
            why_list_is_one_layer,
            layering_obstacles,
            tiles_hold_content_moving_within_space_by_space_index: vec![true],
            swept_reach_in_parent_space_by_space_index: vec![None],
            innermost_composited_band_at_or_around_band: Vec::new(),
        }
        .with_lookups()
    }

    // The lookups that the methods read, built from what the stack holds. Every index the stack holds must be in range.
    fn with_lookups(mut self) -> Self {
        self.innermost_composited_band_at_or_around_band =
            innermost_composited_band_at_or_around_each_band(&self.nodes, &self.bands);
        self
    }

    /// The stack of a plan of more than one layer, as the planner and the compositor both work it out. The layers come
    /// in the order of the walk of the bands: a band's layer below all cuts, then per cut the layers of its barrier,
    /// which are of live content or those of the nested band, and the layer above the cut.
    /// The ordinal of a band in its node counts the bands of the node before it in the walk. The base layer is the first
    /// layer that can hold content where that is the layer below all cuts of a band of a node that covers the viewport,
    /// and the first layer otherwise.
    ///
    /// Every index the plan holds must be in range, each node must come after the node it is in, and the walk must
    /// enter each band of the plan once.
    fn of_layered_plan(
        plan: LayeredPlan,
        spaces: Vec<RasterSpace>,
        tree: &VisualContextTree,
        layering_obstacles: LayeringObstacles,
    ) -> Self {
        let mut nodes: Vec<PlacementNode> = Vec::with_capacity(plan.nodes.len());
        let mut innermost_composited_node_around_by_node_index: Vec<Option<PlacementNodeIndex>> =
            Vec::with_capacity(plan.nodes.len());
        for node in plan.nodes {
            let innermost_composited_node_around = node.parent_index.and_then(|parent_index| {
                if nodes[parent_index.0].composited_effect.is_some() {
                    Some(parent_index)
                } else {
                    innermost_composited_node_around_by_node_index[parent_index.0]
                }
            });
            innermost_composited_node_around_by_node_index.push(innermost_composited_node_around);
            let identity = match (node.composited_effect, node.parent_index) {
                (Some(effect), Some(parent_index)) => {
                    identity_of_composited_node(nodes[parent_index.0].identity, effect)
                }
                _ => {
                    let identity_outside_composited_nodes =
                        identity_of_node(&spaces, node.raster_space_index, &node.composite_clips);
                    match innermost_composited_node_around {
                        Some(composited_node_index) => identity_of_node_inside_composited_node(
                            nodes[composited_node_index.0].identity,
                            identity_outside_composited_nodes,
                        ),
                        None => identity_outside_composited_nodes,
                    }
                }
            };
            nodes.push(PlacementNode {
                identity,
                raster_space_index: node.raster_space_index,
                composite_clips: node.composite_clips,
                composited_effect: node.composited_effect,
                parent_index: node.parent_index,
                reach_in_parent_node: node.reach_in_parent_node,
            });
        }
        let mut bands: Vec<PlacementNodeBand> = Vec::with_capacity(plan.bands.len());
        let mut layers: Vec<StackLayer> = Vec::new();
        let mut next_band_ordinal_by_node_index = vec![0; nodes.len()];
        let mut bands_of_plan = plan.bands.into_iter();
        let mut walk: Vec<BandOfWalk> = Vec::new();
        let mut enters_next_band = true;
        loop {
            if enters_next_band && let Some(band) = bands_of_plan.next() {
                let band_index = BandIndex(bands.len());
                let band_ordinal_in_node = &mut next_band_ordinal_by_node_index[band.placement_node_index.0];
                bands.push(PlacementNodeBand {
                    placement_node_index: band.placement_node_index,
                    band_ordinal_in_node: *band_ordinal_in_node,
                    parent_band_holding_this_band_as_barrier: walk.last().map(|walked| walked.band_index),
                    record_spans: band.record_spans,
                    cuts: Vec::with_capacity(band.cuts.len()),
                    layer_below_all_cuts: LayerIndex(layers.len()),
                });
                *band_ordinal_in_node += 1;
                let layer = painted_layer_of_band(&nodes, &bands, band_index, None, band.reach_of_layer_below_all_cuts);
                layers.push(layer);
                walk.push(BandOfWalk {
                    band_index,
                    cuts_to_pass: band.cuts.into_iter(),
                    reach_of_layer_above_nested_band: IntRect::default(),
                });
            }
            enters_next_band = false;
            let Some(walked) = walk.last_mut() else {
                break;
            };
            let band_index = walked.band_index;
            match walked.cuts_to_pass.next() {
                Some(CutOfPlan {
                    barrier:
                        BarrierOfPlan::Media {
                            content,
                            ordinal,
                            record_span,
                            reach,
                            cuts_band,
                        },
                    reach_of_layer_above_cut,
                }) => {
                    layers.push(StackLayer {
                        identity: LayerIdentity::Media { content, ordinal },
                        placement_node_index: bands[band_index.0].placement_node_index,
                        band_index,
                        content: if content.is_drawn_by_its_record() {
                            LayerContent::OwnSurface { record_span }
                        } else {
                            LayerContent::Media { record_span }
                        },
                        may_hold_content: true,
                        reach,
                    });
                    let barrier = Some(BarrierIdentity::Media { content, ordinal });
                    let layer = painted_layer_of_band(&nodes, &bands, band_index, barrier, reach_of_layer_above_cut);
                    layers.push(layer);
                    bands[band_index.0].cuts.push(BarrierCut {
                        barrier_first_record_position: record_span.begin,
                        cut_reach: if cuts_band {
                            Reach::Within(reach)
                        } else {
                            Reach::Nothing
                        },
                        layer_above_cut: LayerIndex(layers.len() - 1),
                    });
                }
                Some(CutOfPlan {
                    barrier: BarrierOfPlan::NestedBand,
                    reach_of_layer_above_cut,
                }) => {
                    walked.reach_of_layer_above_nested_band = reach_of_layer_above_cut;
                    enters_next_band = true;
                }
                None => {
                    walk.pop();
                    let Some(walked_around) = walk.last() else {
                        continue;
                    };
                    let nested_band = &bands[band_index.0];
                    let nested_node = &nodes[nested_band.placement_node_index.0];
                    // A plan opens a band for the stretch of records it starts with, so the cut of a nested band is
                    // painted where its content begins.
                    let where_content_of_nested_band_begins = [
                        nested_band.record_spans.first().map(|span| span.begin),
                        nested_band.cuts.first().map(|cut| cut.barrier_first_record_position),
                    ]
                    .into_iter()
                    .flatten()
                    .min()
                    .unwrap_or_default();
                    let cut = BarrierCut {
                        barrier_first_record_position: where_content_of_nested_band_begins,
                        cut_reach: nested_node.reach_in_parent_node,
                        layer_above_cut: LayerIndex(layers.len()),
                    };
                    let barrier = Some(BarrierIdentity::NestedNodeBand {
                        node: nested_node.identity,
                        band_ordinal_in_node: nested_band.band_ordinal_in_node,
                    });
                    let (band_index, reach_of_layer_above_cut) =
                        (walked_around.band_index, walked_around.reach_of_layer_above_nested_band);
                    let layer = painted_layer_of_band(&nodes, &bands, band_index, barrier, reach_of_layer_above_cut);
                    layers.push(layer);
                    bands[band_index.0].cuts.push(cut);
                }
            }
        }
        let base_layer = layers
            .iter()
            .position(|layer| layer.may_hold_content)
            .filter(|&index| {
                matches!(
                    layers[index].identity,
                    LayerIdentity::Painted {
                        above_barrier: None,
                        ..
                    }
                ) && node_covers_viewport(&nodes, &spaces, layers[index].placement_node_index)
            })
            .map_or(LayerIndex(0), LayerIndex);
        LayerStack {
            composited_effects: CompositedEffect::of_nodes(&nodes, tree),
            spaces,
            nodes,
            bands,
            layers,
            base_layer,
            why_list_is_one_layer: None,
            layering_obstacles,
            tiles_hold_content_moving_within_space_by_space_index: plan
                .tiles_hold_content_moving_within_space_by_space_index,
            swept_reach_in_parent_space_by_space_index: plan.swept_reach_in_parent_space_by_space_index,
            innermost_composited_band_at_or_around_band: Vec::new(),
        }
        .with_lookups()
    }

    pub fn replays_whole_list(&self) -> bool {
        self.why_list_is_one_layer.is_some()
    }

    /// Whether every tile replays the whole list: a stack of one layer whose tree sorts planes by depth cannot tell
    /// which records show in a tile.
    pub fn one_layer_list_cannot_tell_which_records_show_in_tile(&self) -> bool {
        self.replays_whole_list() && self.layering_obstacles.sorts_planes_by_depth
    }

    /// Whether tiles of the space hold content that moves within it as nodes that move on their own, or that the
    /// compositor scrolls without the space following them, do, so that the tiles read where those nodes are. Those of
    /// a space that holds none read no value the compositor animates either.
    pub fn space_holds_content_that_moves_within_it(&self, raster_space_index: RasterSpaceIndex) -> bool {
        self.replays_whole_list()
            || self
                .tiles_hold_content_moving_within_space_by_space_index
                .get(raster_space_index.0)
                .copied()
                .unwrap_or(true)
    }

    // The layers of a band come in the order of its cuts, so its topmost layer is the one above its last cut.
    pub fn band_has_layer_at_or_above_base_layer(&self, band_index: BandIndex) -> bool {
        let band = &self.bands[band_index.0];
        let topmost_layer_of_band = band
            .cuts
            .last()
            .map_or(band.layer_below_all_cuts, |cut| cut.layer_above_cut);
        topmost_layer_of_band >= self.base_layer
    }

    pub fn space_of_layer(&self, layer_index: LayerIndex) -> RasterSpaceIndex {
        self.nodes[self.layers[layer_index.0].placement_node_index.0].raster_space_index
    }

    // An offset past the coordinate limit shows nothing of the space, and is kept within the limit so that a rect of the
    // stack offset by it still fits in an `IntRect`.
    fn composite_offset_of_space(
        &self,
        raster_space_index: RasterSpaceIndex,
        scroll_offsets: &[FloatPoint],
    ) -> IntPoint {
        let (x, y) = space_and_ancestors_below_viewport(&self.spaces, raster_space_index).fold(
            (0i64, 0i64),
            |(x_so_far, y_so_far), space_or_ancestor_index| {
                let space_or_ancestor = &self.spaces[space_or_ancestor_index.0];
                let anchor_translation = space_or_ancestor.anchor_translation_on_whole_device_pixels(
                    device_offset_for_index(scroll_offsets, space_or_ancestor.anchor),
                );
                (
                    x_so_far + i64::from(anchor_translation.x),
                    y_so_far + i64::from(anchor_translation.y),
                )
            },
        );
        let limit = i64::from(COORDINATE_LIMIT_IN_DEVICE_PIXELS);
        IntPoint {
            x: x.clamp(-limit, limit) as i32,
            y: y.clamp(-limit, limit) as i32,
        }
    }

    /// Per raster space, by its index, where the frame composites its rasters, or for a space whose anchor the
    /// compositor animates, those of the space it is in.
    pub fn composite_offsets(&self, scroll_offsets: &[FloatPoint]) -> Vec<IntPoint> {
        (0..self.spaces.len())
            .map(|space_index| self.composite_offset_of_space(RasterSpaceIndex(space_index), scroll_offsets))
            .collect()
    }

    /// Per raster space, by its index, how the frame composites its rasters: at `composite_offsets`, or for a space
    /// whose anchor the compositor animates, through the matrix `draw_matrix_in_frame` gives its anchor in the frame.
    pub fn spaces_to_frame(
        &self,
        composite_offsets: &[IntPoint],
        draw_matrix_in_frame: impl Fn(SpatialNodeIndex) -> FloatMatrix4x4,
    ) -> Vec<SpaceToFrame> {
        self.spaces
            .iter()
            .zip(composite_offsets)
            .map(|(space, &composite_offset)| match space.anchor_kind {
                RasterSpaceAnchorKind::AnimatedTransform { raster_scale } => {
                    let anchor_to_frame = draw_matrix_in_frame(space.anchor);
                    if !anchor_to_frame.is_2d_affine() {
                        return SpaceToFrame::Offset(composite_offset);
                    }
                    let raster_to_anchor = 1.0 / raster_scale;
                    SpaceToFrame::Transform(
                        anchor_to_frame
                            .extract_2d_affine()
                            .scaled(raster_to_anchor, raster_to_anchor),
                    )
                }
                _ => SpaceToFrame::Offset(composite_offset),
            })
            .collect()
    }

    /// The clip a layer's raster is composited within, in the viewport, or none for no clip.
    pub fn composite_clip_of_layer_in_viewport(
        &self,
        layer_index: LayerIndex,
        composite_offsets: &[IntPoint],
    ) -> Option<IntRect> {
        self.composite_clip_of_node_in_viewport(self.layers[layer_index.0].placement_node_index, composite_offsets)
    }

    /// The clip the rasters of a node are composited within, in the viewport, or none for no clip.
    pub fn composite_clip_of_node_in_viewport(
        &self,
        node_index: PlacementNodeIndex,
        composite_offsets: &[IntPoint],
    ) -> Option<IntRect> {
        self.nodes[node_index.0]
            .composite_clips
            .iter()
            .fold(None, |intersection_so_far, clip| {
                let clip_rect_in_viewport = clip.rect.translated(
                    composite_offsets[clip.raster_space_index.0].x,
                    composite_offsets[clip.raster_space_index.0].y,
                );
                Some(
                    intersection_so_far.map_or(clip_rect_in_viewport, |intersection_so_far: IntRect| {
                        intersection_so_far.intersected(clip_rect_in_viewport)
                    }),
                )
            })
    }

    /// The rounded composite clips of a node, in the viewport, which the clip of `composite_clip_of_node_in_viewport`
    /// bounds.
    pub fn rounded_composite_clips_of_node_in_viewport(
        &self,
        node_index: PlacementNodeIndex,
        composite_offsets: &[IntPoint],
    ) -> Vec<(IntRect, CornerRadii)> {
        self.nodes[node_index.0]
            .composite_clips
            .iter()
            .filter(|clip| clip.is_rounded())
            .map(|clip| {
                let offset = composite_offsets[clip.raster_space_index.0];
                (clip.rect.translated(offset.x, offset.y), clip.corner_radii)
            })
            .collect()
    }

    /// Whether the layer is rastered in a space whose anchor the compositor animates, which the frame composites
    /// through the anchor's transform.
    fn layer_is_in_animated_space(&self, layer_index: LayerIndex) -> bool {
        matches!(
            self.spaces[self.space_of_layer(layer_index).0].anchor_kind,
            RasterSpaceAnchorKind::AnimatedTransform { .. }
        )
    }

    fn layer_is_composited_within_rounded_clips(&self, layer_index: LayerIndex) -> bool {
        self.nodes[self.layers[layer_index.0].placement_node_index.0]
            .composite_clips
            .iter()
            .any(CompositeClip::is_rounded)
    }

    /// The innermost band the layer is in whose node the compositor composites inside its effect, if any.
    fn innermost_composited_band_around_layer(&self, layer_index: LayerIndex) -> Option<BandIndex> {
        self.innermost_composited_band_at_or_around_band[self.layers[layer_index.0].band_index.0]
    }

    /// The next band out from `composited_band` whose node the compositor composites inside its effect, if any.
    fn composited_band_around(&self, composited_band: BandIndex) -> Option<BandIndex> {
        self.bands[composited_band.0]
            .parent_band_holding_this_band_as_barrier
            .and_then(|band_around| self.innermost_composited_band_at_or_around_band[band_around.0])
    }

    pub fn layer_is_inside_composited_effects(&self, layer_index: LayerIndex) -> bool {
        self.innermost_composited_band_around_layer(layer_index).is_some()
    }

    /// The effect the compositor composites the records of the band inside: that of the innermost node at or around the
    /// band's node that it composites inside an effect, which may be a node of another space. A replay of the band's
    /// records leaves it out, with the effects around it and the clips it is entered within.
    pub fn innermost_composited_effect_at_or_around_band(&self, band_index: BandIndex) -> Option<EffectNodeIndex> {
        let composited_band = self.innermost_composited_band_at_or_around_band[band_index.0]?;
        self.nodes[self.bands[composited_band.0].placement_node_index.0].composited_effect
    }

    /// The bands the layer is in, innermost first, whose nodes the compositor composites inside their effects. A plan
    /// nests no more of them than `MAXIMUM_NESTING_OF_COMPOSITED_EFFECTS`.
    pub fn composited_bands_around_layer_innermost_first(
        &self,
        layer_index: LayerIndex,
    ) -> impl Iterator<Item = BandIndex> + '_ {
        std::iter::successors(
            self.innermost_composited_band_around_layer(layer_index),
            |&band_index| self.composited_band_around(band_index),
        )
    }

    /// Per layer, how far the filters of the effects the compositor composites around it read past each of its pixels,
    /// summed and cut to `LARGEST_SUMMED_READ_REACH_OF_COMPOSITED_FILTERS_IN_DEVICE_PIXELS`, where
    /// `read_reach_of_composited_node` tells it of one composited node and its effect.
    pub fn summed_read_reach_of_composited_filters_by_layer(
        &self,
        mut read_reach_of_composited_node: impl FnMut(PlacementNodeIndex, EffectNodeIndex) -> i32,
    ) -> Vec<i32> {
        if self.composited_effects.is_empty() {
            return vec![0; self.layers.len()];
        }
        let mut read_reach_by_composited_node_index: Vec<Option<i32>> = vec![None; self.nodes.len()];
        let mut summed_read_reach_by_band_index: Vec<i32> = Vec::with_capacity(self.bands.len());
        for band in &self.bands {
            let summed_around = band
                .parent_band_holding_this_band_as_barrier
                .and_then(|band_around| summed_read_reach_by_band_index.get(band_around.0).copied())
                .unwrap_or(0);
            let node_index = band.placement_node_index;
            let read_reach_of_node = self.nodes[node_index.0].composited_effect.map_or(0, |effect| {
                *read_reach_by_composited_node_index[node_index.0]
                    .get_or_insert_with(|| read_reach_of_composited_node(node_index, effect))
            });
            summed_read_reach_by_band_index.push(
                summed_around
                    .saturating_add(read_reach_of_node)
                    .min(LARGEST_SUMMED_READ_REACH_OF_COMPOSITED_FILTERS_IN_DEVICE_PIXELS),
            );
        }
        self.layers
            .iter()
            .map(|layer| summed_read_reach_by_band_index[layer.band_index.0])
            .collect()
    }

    // A layer of a space whose anchor the compositor animates is taken to be anywhere the animation can take it, so
    // that which media the window server shows stays put from one sample to the next.
    fn reach_of_layer_in_viewport(&self, layer_index: LayerIndex, composite_offsets: &[IntPoint]) -> IntRect {
        let node = &self.nodes[self.layers[layer_index.0].placement_node_index.0];
        let swept_reach_of_animated_space = self
            .swept_reach_in_parent_space_by_space_index
            .get(node.raster_space_index.0)
            .copied()
            .flatten();
        let mut raster_space_index_of_reach = node.raster_space_index;
        let reach_in_raster_space = match self.composited_bands_around_layer_innermost_first(layer_index).last() {
            _ if let Some(swept_reach) = swept_reach_of_animated_space => {
                raster_space_index_of_reach = self.spaces[node.raster_space_index.0].parent_index;
                swept_reach
            }
            Some(outermost_composited_band) => {
                let outermost_composited_node =
                    &self.nodes[self.bands[outermost_composited_band.0].placement_node_index.0];
                if let Some(node_around_outermost_composited_node) = outermost_composited_node.parent_index {
                    raster_space_index_of_reach =
                        self.nodes[node_around_outermost_composited_node.0].raster_space_index;
                }
                match outermost_composited_node.reach_in_parent_node {
                    Reach::Nothing => return IntRect::default(),
                    Reach::Within(draw_and_read_reach_of_outermost_composited_effect) => {
                        draw_and_read_reach_of_outermost_composited_effect
                    }
                    Reach::Anywhere => return self.spaces[RasterSpaceIndex::VIEWPORT.0].extent,
                }
            }
            None => self.layers[layer_index.0].reach,
        };
        let composite_offset = composite_offsets[raster_space_index_of_reach.0];
        let reach_in_viewport = reach_in_raster_space.translated(composite_offset.x, composite_offset.y);
        match self.composite_clip_of_layer_in_viewport(layer_index, composite_offsets) {
            Some(clip) => reach_in_viewport.intersected(clip),
            None => reach_in_viewport,
        }
    }

    /// Why the frame holds the layer whatever it does with the layers around it, as the window server cannot show the
    /// layer by itself. None for a layer the window server can show.
    fn why_frame_must_hold_layer(
        &self,
        layer_index: LayerIndex,
        window_server: &WhatWindowServerShowsOfFrame<'_>,
    ) -> Option<WhyFrameHoldsLayer> {
        let shows_tiles_of_painted_layers = window_server.compositing.shows_tiles_of_painted_layers;
        let window_server_composites_effects_around_layer = self
            .composited_bands_around_layer_innermost_first(layer_index)
            .all(|band_index| {
                shows_tiles_of_painted_layers
                    && (window_server.can_composite_effect_of_node)(self.bands[band_index.0].placement_node_index)
            });
        let window_server_cannot_show_content = match self.layers[layer_index.0].content {
            LayerContent::Painted => false,
            LayerContent::Media { .. } => !(window_server.can_show_media_of_layer)(layer_index),
            // The window server shows a caret or a scroll bar from a surface of its own as it shows a tile.
            LayerContent::OwnSurface { .. } => !shows_tiles_of_painted_layers,
        };
        if !window_server_composites_effects_around_layer {
            Some(WhyFrameHoldsLayer::InsideAnEffectTheCompositorComposites)
        } else if !shows_tiles_of_painted_layers && self.layer_is_composited_within_rounded_clips(layer_index) {
            Some(WhyFrameHoldsLayer::CompositedWithinRoundedCorners)
        } else if !shows_tiles_of_painted_layers && self.layer_is_in_animated_space(layer_index) {
            Some(WhyFrameHoldsLayer::InSpaceTheCompositorAnimates)
        } else if window_server_cannot_show_content {
            Some(WhyFrameHoldsLayer::WindowServerCannotShowItsContent)
        } else {
            None
        }
    }

    /// Why the frame holds a layer that `layers_the_window_server_shows` does not have the window server show.
    pub fn why_frame_holds_layer(
        &self,
        layer_index: LayerIndex,
        window_server: &WhatWindowServerShowsOfFrame<'_>,
    ) -> WhyFrameHoldsLayer {
        if self.replays_whole_list() {
            return WhyFrameHoldsLayer::ListIsOneLayer;
        }
        self.why_frame_must_hold_layer(layer_index, window_server).unwrap_or(
            if window_server.compositing.shows_tiles_of_painted_layers {
                WhyFrameHoldsLayer::UnderALayerTheFrameHolds
            } else {
                WhyFrameHoldsLayer::WindowServerShowsNoTiles
            },
        )
    }

    /// Which layers the window server shows by itself, rather than the frame holding them, of the base layer and those
    /// above it that can hold content. The frame holds every layer up to the topmost one it must hold of those that can
    /// show in the viewport: one inside an effect the compositor composites, one composited through a transform or
    /// within a clip the window server does not apply, or a canvas, video or nested context the window server cannot
    /// show by itself, as `window_server` says. An effect is composited around all it holds by the frame or by the
    /// window server, so the frame holds every layer inside an effect it holds a layer inside. Below the topmost layer
    /// the frame holds, the window server still shows a canvas or video it can show, that is inside no composited
    /// effect, where no layer above it that the frame holds can show over it.
    ///
    /// A window server that shows the tiles of painted layers shows every layer above the topmost one the frame must
    /// hold, and the base layer too where the frame must hold none, so that the frame holds nothing. Any other shows a
    /// painted layer only to keep it above a canvas or video that it shows, so the frame also holds the painted layers
    /// below the lowest of those, and every layer where the window server shows no canvas or video.
    pub fn layers_the_window_server_shows(
        &self,
        composite_offsets: &[IntPoint],
        window_server: &WhatWindowServerShowsOfFrame<'_>,
    ) -> Vec<bool> {
        let mut shown_by_window_server = vec![false; self.layers.len()];
        let is_media = |layer: LayerIndex| matches!(self.layers[layer.0].content, LayerContent::Media { .. });
        let shows_tiles_of_painted_layers = window_server.compositing.shows_tiles_of_painted_layers;
        let has_media_above_base = (self.base_layer.0 + 1..self.layers.len()).any(|layer| is_media(LayerIndex(layer)));
        if self.replays_whole_list() || !(shows_tiles_of_painted_layers || has_media_above_base) {
            return shown_by_window_server;
        }
        let layers_above_base_that_may_hold_content: Vec<LayerIndex> = (self.base_layer.0 + 1..self.layers.len())
            .map(LayerIndex)
            .filter(|&layer| self.layers[layer.0].may_hold_content)
            .collect();
        let frame_must_hold = |layer: LayerIndex| self.why_frame_must_hold_layer(layer, window_server).is_some();
        // A layer that shows nowhere in the viewport keeps no other layer in the frame, whoever is to show it.
        let viewport_rect = self.spaces[RasterSpaceIndex::VIEWPORT.0].extent;
        let can_show_in_viewport = |layer: LayerIndex| {
            !self
                .reach_of_layer_in_viewport(layer, composite_offsets)
                .intersected(viewport_rect)
                .is_empty()
        };
        let mut position_of_topmost_layer_frame_must_hold = layers_above_base_that_may_hold_content
            .iter()
            .rposition(|&layer| frame_must_hold(layer) && can_show_in_viewport(layer));
        // The layers of a band come one after another, from its layer below all cuts on.
        if let Some(position_of_topmost_layer) = &mut position_of_topmost_layer_frame_must_hold {
            for position in *position_of_topmost_layer + 1..layers_above_base_that_may_hold_content.len() {
                let topmost_layer_frame_holds = layers_above_base_that_may_hold_content[*position_of_topmost_layer];
                let is_inside_effect_frame_holds_layer_inside = self
                    .composited_bands_around_layer_innermost_first(layers_above_base_that_may_hold_content[position])
                    .any(|band_index| self.bands[band_index.0].layer_below_all_cuts <= topmost_layer_frame_holds);
                if is_inside_effect_frame_holds_layer_inside {
                    *position_of_topmost_layer = position;
                }
            }
        }
        let count_up_to_topmost_layer_frame_must_hold =
            position_of_topmost_layer_frame_must_hold.map_or(0, |position| position + 1);
        for &layer in &layers_above_base_that_may_hold_content[count_up_to_topmost_layer_frame_must_hold..] {
            shown_by_window_server[layer.0] = true;
        }
        let mut reach_in_viewport_of_held_layers_above = IntRect::default();
        for &layer in layers_above_base_that_may_hold_content[..count_up_to_topmost_layer_frame_must_hold]
            .iter()
            .rev()
        {
            let reach_in_viewport = self.reach_of_layer_in_viewport(layer, composite_offsets);
            if is_media(layer)
                && !frame_must_hold(layer)
                && !self.layer_is_inside_composited_effects(layer)
                && reach_in_viewport_of_held_layers_above
                    .intersected(reach_in_viewport)
                    .is_empty()
            {
                shown_by_window_server[layer.0] = true;
                continue;
            }
            reach_in_viewport_of_held_layers_above = reach_in_viewport_of_held_layers_above.united(reach_in_viewport);
        }
        if shows_tiles_of_painted_layers {
            shown_by_window_server[self.base_layer.0] = position_of_topmost_layer_frame_must_hold.is_none();
            return shown_by_window_server;
        }
        let lowest_media_the_window_server_shows = layers_above_base_that_may_hold_content
            .iter()
            .copied()
            .find(|&layer| is_media(layer) && shown_by_window_server[layer.0]);
        for &layer in &layers_above_base_that_may_hold_content {
            if lowest_media_the_window_server_shows.is_none_or(|lowest_media| layer < lowest_media) {
                shown_by_window_server[layer.0] = false;
            }
        }
        shown_by_window_server
    }

    /// Works out where the window server can show the content of each canvas and video layer under `scroll_offsets`,
    /// and which layers it shows where it can show every canvas and video it has a placement for.
    pub fn window_server_presentation_of_media(
        &self,
        list: &DisplayListStorage,
        tree: &VisualContextTree,
        scroll_offsets: &[FloatPoint],
        viewport_rect: IntRect,
        window_server_compositing: WindowServerCompositing,
    ) -> WindowServerPresentationOfMedia {
        let world_of_frame = std::cell::OnceCell::new();
        let placements = self.window_server_placements_of_media(
            list,
            tree,
            || {
                world_of_frame
                    .get_or_init(|| WorldTransforms::new(tree, scroll_offsets, Some(FloatMatrix4x4::identity())))
            },
            viewport_rect,
        );
        let placements_of_effects = if window_server_compositing.shows_tiles_of_painted_layers {
            self.window_server_placements_of_composited_effects(
                list,
                tree,
                || {
                    world_of_frame
                        .get_or_init(|| WorldTransforms::new(tree, scroll_offsets, Some(FloatMatrix4x4::identity())))
                },
                window_server_compositing,
            )
        } else {
            WindowServerPlacementsOfEffects::default()
        };
        let composite_offsets = self.composite_offsets(scroll_offsets);
        let mut presentation = WindowServerPresentationOfMedia {
            placements,
            placements_of_effects,
            shown_by_window_server_by_layer_index: Vec::new(),
            window_server_compositing,
        };
        presentation.shown_by_window_server_by_layer_index =
            presentation.with_what_window_server_shows_of_frame(|window_server| {
                self.layers_the_window_server_shows(&composite_offsets, window_server)
            });
        presentation
    }

    /// Works out how the window server could composite each composited effect of the stack under the frame's values,
    /// with the world transforms of the tree in the frame that `world_of_frame` gives, which it asks for only where the
    /// stack has a composited effect.
    pub fn window_server_placements_of_composited_effects<'world>(
        &self,
        list: &DisplayListStorage,
        tree: &VisualContextTree,
        world_of_frame: impl FnOnce() -> &'world WorldTransforms,
        window_server_compositing: WindowServerCompositing,
    ) -> WindowServerPlacementsOfEffects {
        if self.composited_effects.is_empty() {
            return WindowServerPlacementsOfEffects::default();
        }
        let world_of_frame = world_of_frame();
        let effect_clips = list.effect_clip_plan(tree);
        let mut effect_is_composited = vec![false; tree.effect_nodes.len()];
        for composited in &self.composited_effects {
            effect_is_composited[composited.effect.0 as usize] = true;
        }
        let mut placement_by_node_index = vec![None; self.nodes.len()];
        for composited in &self.composited_effects {
            let effect = composited.effect;
            // Each composited effect is entered within the clips between its output clip and that of the nearest
            // composited effect around it.
            let mut effect_around = tree.effect_nodes[effect.0 as usize].parent;
            while !effect_around.is_none() && !effect_is_composited[effect_around.0 as usize] {
                effect_around = tree.effect_nodes[effect_around.0 as usize].parent;
            }
            let clip_entered_outside_effect = if effect_around.is_none() {
                Some(ClipNodeIndex::NONE)
            } else {
                effect_clips.output_clip_if_participating(effect_around)
            };
            let placement = match (
                effect_clips.output_clip_if_participating(effect),
                clip_entered_outside_effect,
            ) {
                (Some(output_clip), Some(clip_entered_outside_effect)) => window_server_placement_of_effect(
                    tree,
                    world_of_frame,
                    effect,
                    output_clip,
                    clip_entered_outside_effect,
                    window_server_compositing.filters_what_is_below_layers,
                ),
                // No record of the list is inside the effect, so nothing tells the clips it is entered within.
                _ => Err(WhyWindowServerCannotCompositeEffect::Clipped(
                    WhyWindowServerCannotShowMedia::ClippedInSpaceTreeDoesNotHave,
                )),
            };
            placement_by_node_index[composited.placement_node_index.0] = Some(placement);
        }
        WindowServerPlacementsOfEffects {
            placement_by_node_index,
        }
    }

    /// Works out where the window server could show each canvas and video of the stack under the frame's values, with
    /// the world transforms of the tree in the frame that `world_of_frame` gives, which it asks for only where the
    /// stack holds a canvas or video the window server may show.
    pub fn window_server_placements_of_media<'world>(
        &self,
        list: &DisplayListStorage,
        tree: &VisualContextTree,
        world_of_frame: impl FnOnce() -> &'world WorldTransforms,
        viewport_rect: IntRect,
    ) -> WindowServerPlacementsOfMedia {
        let window_server_may_show_content = |content: &LiveContent| {
            !matches!(content, LiveContent::CompositedContext(_)) && !content.is_drawn_by_its_record()
        };
        let has_media_the_window_server_may_show = self.layers.iter().any(|layer| {
            matches!(&layer.identity, LayerIdentity::Media { content, .. } if window_server_may_show_content(content))
        });
        if !has_media_the_window_server_may_show {
            return WindowServerPlacementsOfMedia::default();
        }
        let world_of_frame = world_of_frame();
        let placement_by_layer_index = self
            .layers
            .iter()
            .map(|layer| {
                let (LayerContent::Media { record_span }, LayerIdentity::Media { content, .. }) =
                    (&layer.content, &layer.identity)
                else {
                    return None;
                };
                if !window_server_may_show_content(content) {
                    return None;
                }
                let media_record_position = record_span.begin;
                let chunk = list.chunk(media_record_position.chunk_index);
                let run_holding_media =
                    &chunk.runs()[chunk.index_of_first_run_ending_after(media_record_position.offset)];
                let (header, payload) = list.record_at(media_record_position);
                Some(crate::display_list::window_server_placement::window_server_placement(
                    tree,
                    world_of_frame,
                    viewport_rect,
                    run_holding_media,
                    &header,
                    payload,
                ))
            })
            .collect();
        WindowServerPlacementsOfMedia {
            placement_by_layer_index,
        }
    }

    fn name_of_space(&self, raster_space_index: RasterSpaceIndex, node_namer: &dyn VisualContextNodeNamer) -> String {
        let raster_space = &self.spaces[raster_space_index.0];
        let anchor_kind_name = match raster_space.anchor_kind {
            RasterSpaceAnchorKind::Viewport => return "viewport".into(),
            RasterSpaceAnchorKind::ViewportScroller => return "document".into(),
            RasterSpaceAnchorKind::Scroller => "scroller",
            RasterSpaceAnchorKind::StickyBox { .. } => "sticky",
            RasterSpaceAnchorKind::AnimatedTransform { .. } => "animated",
        };
        format!(
            "{anchor_kind_name}@{} in {}",
            node_namer.name_of_spatial_node(raster_space.anchor),
            self.name_of_space(raster_space.parent_index, node_namer)
        )
    }

    fn name_of_node(&self, node_index: PlacementNodeIndex, node_namer: &dyn VisualContextNodeNamer) -> String {
        let mut name = self.name_of_space(self.nodes[node_index.0].raster_space_index, node_namer);
        let mut node_on_way_out = Some(node_index);
        while let Some(index) = node_on_way_out {
            if let Some(effect) = self.nodes[index.0].composited_effect {
                name.push_str(&format!(" inside {}", node_namer.name_of_effect_node(effect)));
            }
            node_on_way_out = self.nodes[index.0].parent_index;
        }
        name
    }

    // The layer right below a layer above a cut is the layer of the cut's live content, or the topmost layer of the cut's
    // nested band.
    fn name_of_barrier_below(&self, layer_index: LayerIndex, node_namer: &dyn VisualContextNodeNamer) -> String {
        let layer_below = &self.layers[layer_index.0 - 1];
        match layer_below.identity {
            LayerIdentity::Media { .. } => format!("L{}", layer_index.0 - 1),
            LayerIdentity::Painted { .. } => {
                let nested_band = &self.bands[layer_below.band_index.0];
                format!(
                    "band {} of {}",
                    nested_band.band_ordinal_in_node,
                    self.name_of_node(nested_band.placement_node_index, node_namer)
                )
            }
        }
    }

    /// What the dump calls a layer: its index, what it holds, the space it is rastered in and the effects the
    /// compositor composites it inside, and what it sits above.
    pub fn name_of_layer(&self, layer_index: LayerIndex, node_namer: &dyn VisualContextNodeNamer) -> String {
        if self.why_list_is_one_layer.is_some() {
            return "[L0] single layer replaying the whole list".into();
        }
        let index = layer_index.0;
        let layer = &self.layers[index];
        let node_name = self.name_of_node(layer.placement_node_index, node_namer);
        match (&layer.identity, &layer.content) {
            (
                LayerIdentity::Painted {
                    band_ordinal_in_node,
                    above_barrier,
                    ..
                },
                _,
            ) => {
                let kind = if layer_index == self.base_layer {
                    "base"
                } else {
                    "painted"
                };
                let mut name = format!("[L{index}] {kind} space={node_name}");
                if *band_ordinal_in_node != 0 {
                    name.push_str(&format!(" band={band_ordinal_in_node}"));
                }
                if above_barrier.is_some() {
                    name.push_str(&format!(
                        " above {}",
                        self.name_of_barrier_below(layer_index, node_namer)
                    ));
                }
                name
            }
            (LayerIdentity::Media { content, ordinal }, _) => {
                let reach = layer.reach;
                format!(
                    "[L{index}] {} #{ordinal} space={node_name} region=[{},{} {}x{}]",
                    media_name(*content),
                    reach.x,
                    reach.y,
                    reach.width,
                    reach.height
                )
            }
        }
    }
}

/// What of a tree and its list keeps the tiles of its layers from replaying only the records that reach them, or keeps
/// the list from being layered, at some sample of the animations of the tree.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LayeringObstacles {
    pub sorts_planes_by_depth: bool,
}

impl LayeringObstacles {
    pub fn of(tree: &VisualContextTree) -> Self {
        Self {
            sorts_planes_by_depth: tree.may_sort_planes_by_depth(),
        }
    }
}

fn anchors_of_layer_plans(tree: &VisualContextTree) -> Vec<SpatialNodeIndex> {
    raster_space_anchors(tree, &spatial_nodes_that_move_on_their_own(tree))
}
