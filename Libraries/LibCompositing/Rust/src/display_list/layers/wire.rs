/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A plan of the layers of a list as it goes from WebContent to the compositor: what of its stack the compositor cannot
//! work out of the list and the tree, the notes the overlay of layers shows, and the checks that a plan from another
//! process passes before anything reads it.

use super::{
    BandOfPlan, BarrierOfPlan, COORDINATE_LIMIT_IN_DEVICE_PIXELS, CutOfPlan, LayerContent, LayerIdentity, LayerStack,
    LayeredPlan, LayeringObstacles, MAXIMUM_LAYER_COUNT_OF_LIVE_CONTENT_DRAWN_BY_ITS_RECORD,
    MAXIMUM_NESTING_OF_COMPOSITED_EFFECTS, NodeOfPlan, PlacementNodeIndex, StackLayer,
    WhyCompositorCannotCompositeEffect, WhyListIsOneLayer, anchors_of_layer_plans,
};
use crate::display_list::commands::{
    CanvasId, CompositorContextId, EffectNodeIndex, ImageFrameResourceId, SpatialNodeIndex, VideoSinkHandle,
};
use crate::display_list::placement::{
    CompositeClip, MAXIMUM_RASTER_SPACE_COUNT, RasterSpace, RasterSpaceAnchorKind, RasterSpaceIndex, RasterSpaces,
};
use crate::display_list::reach::Reach;
use crate::display_list::storage::{ChunkIndexInList, DisplayListStorage, ListPosition, ListSpan};
use crate::display_list::summary::{LiveContent, live_content_of};
use crate::display_list::tape::DisplayListCommandRun;
use crate::visual_context::VisualContextTree;
use crate::visual_context::queries::NestingOfLiveEffects;
use libgfx_rust::IntRect;
use libipc_rust::wire::{Decode, DecodeError, Decoder, Encode, EncodeError, Encoder};
use std::collections::{HashMap, HashSet};

// Caps on a received stack, which bound what decoding allocates. The values per space, and the composite clips of a node,
// which are one per space, are capped at the most spaces a plan makes. A plan holds nodes, bands, cuts and spans as many
// as the runs of a list ask for, and the planner gives a list one layer where they are more than these caps. Keying a
// cell looks through every cut of its band and keeps those that cut it, so the cuts of a band are capped lower: the
// pages of the tests cut a band at most 90 times.
const MAXIMUM_NODE_COUNT_OF_RECEIVED_STACK: usize = 1 << 16;
const MAXIMUM_BAND_COUNT_OF_RECEIVED_STACK: usize = 1 << 16;
const MAXIMUM_CUT_COUNT_OF_RECEIVED_BAND: usize = 256;
const MAXIMUM_SPAN_COUNT_OF_RECEIVED_BAND: usize = 1 << 20;
/// The most canvases, videos and nested contexts that get layers where giving each one a layer cuts a band more times
/// than the compositor decodes: the cuts a band leaves after the most carets and scroll bars that get layers.
pub(super) const CANVAS_VIDEO_AND_NESTED_CONTEXT_LAYERS_A_BAND_LEAVES_CUTS_FOR: usize =
    MAXIMUM_CUT_COUNT_OF_RECEIVED_BAND - MAXIMUM_LAYER_COUNT_OF_LIVE_CONTENT_DRAWN_BY_ITS_RECORD;
// The most note lines a plan holds, and the most bytes a line holds. A plan holds only the first lines, each cut to that
// length, so that no plan exceeds the caps.
const MAXIMUM_NOTE_LINE_COUNT_OF_PLAN: usize = 1 << 10;
const MAXIMUM_NOTE_LINE_LENGTH_OF_PLAN: usize = 1 << 10;

/// The note lines a plan holds of `lines`: as many of the first lines as a plan holds, each cut at a character boundary
/// to the length a plan holds.
pub(super) fn note_lines_a_plan_holds(lines: impl Iterator<Item = String>) -> Vec<String> {
    lines
        .take(MAXIMUM_NOTE_LINE_COUNT_OF_PLAN)
        .map(|mut line| {
            line.truncate(line.floor_char_boundary(MAXIMUM_NOTE_LINE_LENGTH_OF_PLAN));
            line
        })
        .collect()
}

/// A stack decoded from another process: why it is one layer that replays the whole list, or the plan of a stack of
/// more layers. Nothing reads it until `validated_stack` checks it and works out the stack.
#[derive(Debug)]
enum StackOnWire {
    OneLayer(WhyListIsOneLayer),
    Layered(LayeredPlan),
}

/// A plan of the layers of a list as WebContent sends it to the compositor: the stack, the display list it was planned
/// for, the viewport, in device pixels, it was planned within, and what the overlay of layers tells of the stack past
/// its layers.
#[derive(Clone, Debug, PartialEq)]
pub struct LayerPlan {
    pub display_list_id: u64,
    pub viewport_rect: IntRect,
    pub stack: LayerStack,
    pub note_lines: Vec<String>,
}

/// A plan of layers decoded from another process, whose stack nothing reads until it is checked.
#[derive(Debug)]
pub struct UnvalidatedLayerPlan {
    pub display_list_id: u64,
    pub viewport_rect: IntRect,
    stack: StackOnWire,
    note_lines: Vec<String>,
}

impl UnvalidatedLayerPlan {
    /// The plan, once its stack passes `validated_stack` for `list` under `tree` within the viewport the plan
    /// names. The receiver makes sure that `list` is the list that `display_list_id` names. The note lines go unchecked:
    /// decoding bounds their count and length, and only the overlay of layers shows them.
    pub fn validated(self, list: &DisplayListStorage, tree: &VisualContextTree) -> Result<LayerPlan, &'static str> {
        Ok(LayerPlan {
            display_list_id: self.display_list_id,
            viewport_rect: self.viewport_rect,
            stack: validated_stack(self.stack, list, tree, self.viewport_rect)?,
            note_lines: self.note_lines,
        })
    }
}

macro_rules! impl_codec_of_index_going_as_u32 {
    ($($type:ident),*) => {
        $(
            impl Encode for $type {
                fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
                    encoder.encode(&u32::try_from(self.0).map_err(|_| EncodeError::SizeTooLarge(self.0))?)
                }
            }

            impl Decode for $type {
                const MIN_ENCODED_SIZE: usize = size_of::<u32>();

                fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
                    Ok(Self(decoder.decode::<u32>()? as usize))
                }
            }
        )*
    };
}

impl_codec_of_index_going_as_u32!(RasterSpaceIndex, PlacementNodeIndex);

impl Encode for ListPosition {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.chunk_index.0)?;
        encoder.encode(&self.offset)
    }
}

impl Decode for ListPosition {
    const MIN_ENCODED_SIZE: usize = 2 * size_of::<u32>();

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self::new(ChunkIndexInList(decoder.decode()?), decoder.decode()?))
    }
}

impl Encode for ListSpan {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.begin)?;
        encoder.encode(&self.end)
    }
}

impl Decode for ListSpan {
    const MIN_ENCODED_SIZE: usize = 2 * ListPosition::MIN_ENCODED_SIZE;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self {
            begin: decoder.decode()?,
            end: decoder.decode()?,
        })
    }
}

const REACH_KIND_NOTHING: u8 = 0;
const REACH_KIND_WITHIN: u8 = 1;
const REACH_KIND_ANYWHERE: u8 = 2;

impl Encode for Reach<IntRect> {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        match self {
            Reach::Nothing => encoder.encode(&REACH_KIND_NOTHING),
            Reach::Within(rect) => {
                encoder.encode(&REACH_KIND_WITHIN)?;
                encoder.encode(rect)
            }
            Reach::Anywhere => encoder.encode(&REACH_KIND_ANYWHERE),
        }
    }
}

impl Decode for Reach<IntRect> {
    const MIN_ENCODED_SIZE: usize = size_of::<u8>();

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(match decoder.decode::<u8>()? {
            REACH_KIND_NOTHING => Reach::Nothing,
            REACH_KIND_WITHIN => Reach::Within(decoder.decode()?),
            REACH_KIND_ANYWHERE => Reach::Anywhere,
            _ => return Err(DecodeError::InvalidValue("Reach")),
        })
    }
}

const LIVE_CONTENT_KIND_CANVAS: u8 = 0;
const LIVE_CONTENT_KIND_VIDEO_FRAME: u8 = 1;
const LIVE_CONTENT_KIND_IMAGE_FRAME: u8 = 2;
const LIVE_CONTENT_KIND_COMPOSITED_CONTEXT: u8 = 3;
const LIVE_CONTENT_KIND_CARET: u8 = 4;
const LIVE_CONTENT_KIND_SCROLL_BAR: u8 = 5;

impl Encode for LiveContent {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        let (kind, id) = match self {
            LiveContent::Canvas(id) => (LIVE_CONTENT_KIND_CANVAS, id.0),
            LiveContent::VideoFrame(id) => (LIVE_CONTENT_KIND_VIDEO_FRAME, id.0),
            LiveContent::ImageFrame(id) => (LIVE_CONTENT_KIND_IMAGE_FRAME, id.0),
            LiveContent::CompositedContext(id) => (LIVE_CONTENT_KIND_COMPOSITED_CONTEXT, id.0),
            LiveContent::Caret => (LIVE_CONTENT_KIND_CARET, 0),
            LiveContent::ScrollBar { scroll_node, vertical } => (
                LIVE_CONTENT_KIND_SCROLL_BAR,
                (u64::from(scroll_node.0) << 1) | u64::from(*vertical),
            ),
        };
        encoder.encode(&kind)?;
        encoder.encode(&id)
    }
}

impl Decode for LiveContent {
    const MIN_ENCODED_SIZE: usize = size_of::<u8>() + size_of::<u64>();

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        let kind = decoder.decode::<u8>()?;
        let id = decoder.decode::<u64>()?;
        Ok(match kind {
            LIVE_CONTENT_KIND_CANVAS => LiveContent::Canvas(CanvasId(id)),
            LIVE_CONTENT_KIND_VIDEO_FRAME => LiveContent::VideoFrame(VideoSinkHandle(id)),
            LIVE_CONTENT_KIND_IMAGE_FRAME => LiveContent::ImageFrame(ImageFrameResourceId(id)),
            LIVE_CONTENT_KIND_COMPOSITED_CONTEXT => LiveContent::CompositedContext(CompositorContextId(id)),
            LIVE_CONTENT_KIND_CARET if id == 0 => LiveContent::Caret,
            LIVE_CONTENT_KIND_SCROLL_BAR if id >> 33 == 0 => LiveContent::ScrollBar {
                scroll_node: SpatialNodeIndex((id >> 1) as u32),
                vertical: id & 1 != 0,
            },
            _ => return Err(DecodeError::InvalidValue("LiveContent")),
        })
    }
}

impl Encode for CompositeClip {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.raster_space_index)?;
        encoder.encode(&self.rect)?;
        encoder.encode(&self.corner_radii)
    }
}

impl Decode for CompositeClip {
    const MIN_ENCODED_SIZE: usize =
        RasterSpaceIndex::MIN_ENCODED_SIZE + IntRect::MIN_ENCODED_SIZE + libgfx_rust::CornerRadii::MIN_ENCODED_SIZE;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self {
            raster_space_index: decoder.decode()?,
            rect: decoder.decode()?,
            corner_radii: decoder.decode()?,
        })
    }
}

impl Decode for NodeOfPlan {
    const MIN_ENCODED_SIZE: usize = RasterSpaceIndex::MIN_ENCODED_SIZE
        + Vec::<CompositeClip>::MIN_ENCODED_SIZE
        + Option::<EffectNodeIndex>::MIN_ENCODED_SIZE
        + Option::<PlacementNodeIndex>::MIN_ENCODED_SIZE
        + Reach::<IntRect>::MIN_ENCODED_SIZE;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self {
            raster_space_index: decoder.decode()?,
            composite_clips: decoder.decode_with_max_size(MAXIMUM_RASTER_SPACE_COUNT)?,
            composited_effect: decoder.decode()?,
            parent_index: decoder.decode()?,
            reach_in_parent_node: decoder.decode()?,
        })
    }
}

const BARRIER_KIND_MEDIA: u8 = 0;
const BARRIER_KIND_NESTED_BAND: u8 = 1;

impl Decode for CutOfPlan {
    const MIN_ENCODED_SIZE: usize = size_of::<u8>() + IntRect::MIN_ENCODED_SIZE;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        let barrier = match decoder.decode::<u8>()? {
            BARRIER_KIND_MEDIA => BarrierOfPlan::Media {
                content: decoder.decode()?,
                ordinal: decoder.decode()?,
                record_span: decoder.decode()?,
                reach: decoder.decode()?,
                cuts_band: decoder.decode()?,
            },
            BARRIER_KIND_NESTED_BAND => BarrierOfPlan::NestedBand,
            _ => return Err(DecodeError::InvalidValue("BarrierOfPlan")),
        };
        Ok(Self {
            barrier,
            reach_of_layer_above_cut: decoder.decode()?,
        })
    }
}

impl Decode for BandOfPlan {
    const MIN_ENCODED_SIZE: usize = PlacementNodeIndex::MIN_ENCODED_SIZE
        + Vec::<ListSpan>::MIN_ENCODED_SIZE
        + IntRect::MIN_ENCODED_SIZE
        + Vec::<CutOfPlan>::MIN_ENCODED_SIZE;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self {
            placement_node_index: decoder.decode()?,
            record_spans: decoder.decode_with_max_size(MAXIMUM_SPAN_COUNT_OF_RECEIVED_BAND)?,
            reach_of_layer_below_all_cuts: decoder.decode()?,
            cuts: decoder.decode_with_max_size(MAXIMUM_CUT_COUNT_OF_RECEIVED_BAND)?,
        })
    }
}

impl Encode for WhyCompositorCannotCompositeEffect {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        let kind: u8 = match self {
            WhyCompositorCannotCompositeEffect::FilterMovesWhatItFilters => 0,
            WhyCompositorCannotCompositeEffect::FilterReadsFartherThanTilesAreFollowed => 1,
        };
        encoder.encode(&kind)
    }
}

impl Decode for WhyCompositorCannotCompositeEffect {
    const MIN_ENCODED_SIZE: usize = size_of::<u8>();

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(match decoder.decode::<u8>()? {
            0 => WhyCompositorCannotCompositeEffect::FilterMovesWhatItFilters,
            1 => WhyCompositorCannotCompositeEffect::FilterReadsFartherThanTilesAreFollowed,
            _ => return Err(DecodeError::InvalidValue("WhyCompositorCannotCompositeEffect")),
        })
    }
}

impl Encode for WhyListIsOneLayer {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        let kind: u8 = match self {
            WhyListIsOneLayer::EmptyViewport => 0,
            WhyListIsOneLayer::EmptyDisplayList => 1,
            WhyListIsOneLayer::SortingContext3D => 2,
            WhyListIsOneLayer::RootIsolationEffectIsNotPlainGroup => 3,
            WhyListIsOneLayer::RecordsBlendWithWhatOtherLayersHoldBelowThem => 5,
            WhyListIsOneLayer::EffectTheCompositorCannotCompositeIsComposited(_) => 6,
            WhyListIsOneLayer::FrameIsNotDrawnAsPlanned => 7,
            WhyListIsOneLayer::CompositedEffectsNestTooDeep => 8,
            WhyListIsOneLayer::PlanShowsTooManyTiles => 9,
            WhyListIsOneLayer::PlanFailsChecksOfCompositor => 10,
            WhyListIsOneLayer::PlanHoldsMoreThanCompositorDecodes => 11,
        };
        encoder.encode(&kind)?;
        if let WhyListIsOneLayer::EffectTheCompositorCannotCompositeIsComposited(why) = self {
            encoder.encode(why)?;
        }
        Ok(())
    }
}

impl Decode for WhyListIsOneLayer {
    const MIN_ENCODED_SIZE: usize = size_of::<u8>();

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(match decoder.decode::<u8>()? {
            0 => WhyListIsOneLayer::EmptyViewport,
            1 => WhyListIsOneLayer::EmptyDisplayList,
            2 => WhyListIsOneLayer::SortingContext3D,
            3 => WhyListIsOneLayer::RootIsolationEffectIsNotPlainGroup,
            5 => WhyListIsOneLayer::RecordsBlendWithWhatOtherLayersHoldBelowThem,
            6 => WhyListIsOneLayer::EffectTheCompositorCannotCompositeIsComposited(decoder.decode()?),
            7 => WhyListIsOneLayer::FrameIsNotDrawnAsPlanned,
            8 => WhyListIsOneLayer::CompositedEffectsNestTooDeep,
            9 => WhyListIsOneLayer::PlanShowsTooManyTiles,
            10 => WhyListIsOneLayer::PlanFailsChecksOfCompositor,
            11 => WhyListIsOneLayer::PlanHoldsMoreThanCompositorDecodes,
            _ => return Err(DecodeError::InvalidValue("WhyListIsOneLayer")),
        })
    }
}

const PLAN_KIND_ONE_LAYER: u8 = 0;
const PLAN_KIND_LAYERED: u8 = 1;

impl Decode for StackOnWire {
    const MIN_ENCODED_SIZE: usize = size_of::<u8>() + WhyListIsOneLayer::MIN_ENCODED_SIZE;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(match decoder.decode::<u8>()? {
            PLAN_KIND_ONE_LAYER => StackOnWire::OneLayer(decoder.decode()?),
            PLAN_KIND_LAYERED => StackOnWire::Layered(LayeredPlan {
                nodes: decoder.decode_with_max_size(MAXIMUM_NODE_COUNT_OF_RECEIVED_STACK)?,
                bands: decoder.decode_with_max_size(MAXIMUM_BAND_COUNT_OF_RECEIVED_STACK)?,
                tiles_hold_content_moving_within_space_by_space_index: decoder
                    .decode_with_max_size(MAXIMUM_RASTER_SPACE_COUNT)?,
                swept_reach_in_parent_space_by_space_index: decoder.decode_with_max_size(MAXIMUM_RASTER_SPACE_COUNT)?,
            }),
            _ => return Err(DecodeError::InvalidValue("StackOnWire")),
        })
    }
}

// A stack of one layer that replays the whole list goes as why it is one layer. A stack of more layers goes as its
// plan: its nodes without their identities, and its bands in the order of the walk that its layers are in, each with
// what only it knows of its layers. In that order, the layer of live content comes right below the layer above its cut,
// and the last layer of a nested band comes right below the layer above the cut that enters it.
impl Encode for LayerStack {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        if let Some(why_list_is_one_layer) = self.why_list_is_one_layer {
            encoder.encode(&PLAN_KIND_ONE_LAYER)?;
            return encoder.encode(&why_list_is_one_layer);
        }
        encoder.encode(&PLAN_KIND_LAYERED)?;
        encoder.encode_size(self.nodes.len())?;
        for node in &self.nodes {
            encoder.encode(&node.raster_space_index)?;
            encoder.encode(&node.composite_clips)?;
            encoder.encode(&node.composited_effect)?;
            encoder.encode(&node.parent_index)?;
            encoder.encode(&node.reach_in_parent_node)?;
        }
        encoder.encode_size(self.bands.len())?;
        for band in &self.bands {
            encoder.encode(&band.placement_node_index)?;
            encoder.encode(&band.record_spans)?;
            encoder.encode(&self.layers[band.layer_below_all_cuts.0].reach)?;
            encoder.encode_size(band.cuts.len())?;
            for cut in &band.cuts {
                match &self.layers[cut.layer_above_cut.0 - 1] {
                    StackLayer {
                        identity: LayerIdentity::Media { content, ordinal },
                        content: LayerContent::Media { record_span } | LayerContent::OwnSurface { record_span },
                        reach,
                        ..
                    } => {
                        encoder.encode(&BARRIER_KIND_MEDIA)?;
                        encoder.encode(content)?;
                        encoder.encode(ordinal)?;
                        encoder.encode(record_span)?;
                        encoder.encode(reach)?;
                        encoder.encode(&(cut.cut_reach != Reach::Nothing))?;
                    }
                    _ => encoder.encode(&BARRIER_KIND_NESTED_BAND)?,
                }
                encoder.encode(&self.layers[cut.layer_above_cut.0].reach)?;
            }
        }
        encoder.encode(&self.tiles_hold_content_moving_within_space_by_space_index)?;
        encoder.encode(&self.swept_reach_in_parent_space_by_space_index)
    }
}

fn decode_note_lines(decoder: &mut Decoder<'_>) -> Result<Vec<String>, DecodeError> {
    let count = decoder.decode_size()?;
    if count > MAXIMUM_NOTE_LINE_COUNT_OF_PLAN || count > decoder.remaining_bytes() / String::MIN_ENCODED_SIZE {
        return Err(DecodeError::SizeTooLarge(count));
    }
    (0..count)
        .map(|_| decoder.decode_with_max_size(MAXIMUM_NOTE_LINE_LENGTH_OF_PLAN))
        .collect()
}

impl Encode for LayerPlan {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.display_list_id)?;
        encoder.encode(&self.viewport_rect)?;
        encoder.encode(&self.stack)?;
        encoder.encode(&self.note_lines)
    }
}

impl Decode for UnvalidatedLayerPlan {
    const MIN_ENCODED_SIZE: usize =
        size_of::<u64>() + IntRect::MIN_ENCODED_SIZE + StackOnWire::MIN_ENCODED_SIZE + Vec::<String>::MIN_ENCODED_SIZE;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self {
            display_list_id: decoder.decode()?,
            viewport_rect: decoder.decode()?,
            stack: decoder.decode()?,
            note_lines: decode_note_lines(decoder)?,
        })
    }
}

fn rect_is_within_coordinate_limit(rect: IntRect) -> bool {
    let limit = i64::from(COORDINATE_LIMIT_IN_DEVICE_PIXELS);
    let (x, y, width, height) = (
        i64::from(rect.x),
        i64::from(rect.y),
        i64::from(rect.width),
        i64::from(rect.height),
    );
    width >= 0 && height >= 0 && x >= -limit && y >= -limit && x + width <= limit && y + height <= limit
}

fn reach_is_within_coordinate_limit(reach: Reach<IntRect>) -> bool {
    match reach {
        Reach::Within(rect) => rect_is_within_coordinate_limit(rect),
        Reach::Nothing | Reach::Anywhere => true,
    }
}

fn run_starting_at(list: &DisplayListStorage, position: ListPosition) -> Option<&DisplayListCommandRun> {
    list.chunks()
        .get(position.chunk_index.0 as usize)?
        .run_starting_at(position.offset)
}

fn position_starts_run(list: &DisplayListStorage, position: ListPosition) -> bool {
    run_starting_at(list, position).is_some()
}

fn position_ends_run(list: &DisplayListStorage, position: ListPosition) -> bool {
    let Some(chunk) = list.chunks().get(position.chunk_index.0 as usize) else {
        return false;
    };
    let runs_ending_at_or_before_position = &chunk.runs()[..chunk.index_of_first_run_ending_after(position.offset)];
    runs_ending_at_or_before_position
        .last()
        .is_some_and(|run| run.offset.checked_add(run.size) == Some(position.offset))
}

fn span_is_of_whole_runs(list: &DisplayListStorage, span: ListSpan) -> bool {
    span.begin <= span.end && position_starts_run(list, span.begin) && position_ends_run(list, span.end)
}

// The span of live content with a layer is one run of the list, which starts with a record that draws that content.
fn span_is_run_drawing_content(list: &DisplayListStorage, span: ListSpan, content: LiveContent) -> bool {
    if matches!(content, LiveContent::ImageFrame(_)) {
        return false;
    }
    let Some(run) = run_starting_at(list, span.begin) else {
        return false;
    };
    if run.size == 0 || span.end != ListPosition::new(span.begin.chunk_index, run.offset.saturating_add(run.size)) {
        return false;
    }
    let (header, payload) = list.record_at(span.begin);
    live_content_of(header.command_type, payload) == Some(content)
}

/// Whether `plan` holds more nodes or bands, or a band of it more cuts or spans, than the compositor decodes.
pub(super) fn plan_holds_more_than_compositor_decodes(plan: &LayeredPlan) -> bool {
    plan.nodes.len() > MAXIMUM_NODE_COUNT_OF_RECEIVED_STACK
        || plan.bands.len() > MAXIMUM_BAND_COUNT_OF_RECEIVED_STACK
        || plan.bands.iter().any(|band| {
            band.cuts.len() > MAXIMUM_CUT_COUNT_OF_RECEIVED_BAND
                || band.record_spans.len() > MAXIMUM_SPAN_COUNT_OF_RECEIVED_BAND
        })
}

fn check_viewport_rect(viewport_rect: IntRect) -> Result<(), &'static str> {
    if !rect_is_within_coordinate_limit(viewport_rect) {
        return Err("the viewport of the plan lies past the limit a plan keeps to");
    }
    Ok(())
}

fn check_rects_and_coordinates(plan: &LayeredPlan) -> Result<(), &'static str> {
    let within = rect_is_within_coordinate_limit;
    let radius_limit = i64::from(COORDINATE_LIMIT_IN_DEVICE_PIXELS);
    let radius_is_within = |radius: i32| i64::from(radius) >= 0 && i64::from(radius) <= radius_limit;
    let nodes_are_within = plan.nodes.iter().all(|node| {
        reach_is_within_coordinate_limit(node.reach_in_parent_node)
            && node.composite_clips.iter().all(|clip| {
                let radii = clip.corner_radii;
                within(clip.rect)
                    && [radii.top_left, radii.top_right, radii.bottom_right, radii.bottom_left]
                        .iter()
                        .all(|corner| {
                            radius_is_within(corner.horizontal_radius) && radius_is_within(corner.vertical_radius)
                        })
            })
    });
    let layers_are_within = plan.bands.iter().all(|band| {
        within(band.reach_of_layer_below_all_cuts)
            && band.cuts.iter().all(|cut| {
                within(cut.reach_of_layer_above_cut)
                    && match cut.barrier {
                        BarrierOfPlan::Media { reach, .. } => within(reach),
                        BarrierOfPlan::NestedBand => true,
                    }
            })
    });
    let swept_reaches_are_within = plan
        .swept_reach_in_parent_space_by_space_index
        .iter()
        .flatten()
        .all(|swept_reach| within(*swept_reach));
    if !nodes_are_within || !layers_are_within || !swept_reaches_are_within {
        return Err("a rect or a coordinate of the stack lies past the limit a plan keeps to");
    }
    Ok(())
}

fn check_values_per_space(plan: &LayeredPlan, spaces: &[RasterSpace]) -> Result<(), &'static str> {
    if plan.tiles_hold_content_moving_within_space_by_space_index.len() != spaces.len()
        || plan.swept_reach_in_parent_space_by_space_index.len() != spaces.len()
    {
        return Err("a value the stack holds per space is not there for every space");
    }
    let only_animated_spaces_are_swept =
        spaces
            .iter()
            .zip(&plan.swept_reach_in_parent_space_by_space_index)
            .all(|(space, swept_reach)| {
                swept_reach.is_none() || matches!(space.anchor_kind, RasterSpaceAnchorKind::AnimatedTransform { .. })
            });
    if !only_animated_spaces_are_swept {
        return Err("a space whose anchor the compositor does not animate has a swept reach");
    }
    Ok(())
}

fn check_nodes(
    nodes: &[NodeOfPlan],
    spaces: &[RasterSpace],
    list: &DisplayListStorage,
    tree: &VisualContextTree,
) -> Result<(), &'static str> {
    let mut effects_of_composited_nodes = HashSet::new();
    for (index, node) in nodes.iter().enumerate() {
        if node.raster_space_index.0 >= spaces.len() {
            return Err("a node is in a space the stack does not hold");
        }
        if node.parent_index.is_some_and(|parent_index| parent_index.0 >= index) {
            return Err("a node is not after the node it is in");
        }
        // A node that is not composited is in the node of the space its space is in, or in a composited node of its own
        // space, composited within clips the composited node is not. A composited node is in a node of its own space.
        // So nodes nest no deeper than twice the spaces, plus composited effects, which nest as the tree nests them.
        let parent = node.parent_index.map(|parent_index| &nodes[parent_index.0]);
        let is_in_node_a_plan_puts_it_in = match (node.composited_effect, parent) {
            (None, None) => node.raster_space_index == RasterSpaceIndex::VIEWPORT,
            (None, Some(parent)) => {
                (node.raster_space_index != RasterSpaceIndex::VIEWPORT
                    && parent.raster_space_index == spaces[node.raster_space_index.0].parent_index)
                    || (parent.composited_effect.is_some()
                        && parent.raster_space_index == node.raster_space_index
                        && parent.composite_clips != node.composite_clips)
            }
            (Some(_), Some(parent)) => parent.raster_space_index == node.raster_space_index,
            (Some(_), None) => return Err("a composited node is in no other node"),
        };
        if !is_in_node_a_plan_puts_it_in {
            return Err("a node is not in the node of its space or of the space its space is in");
        }
        let clips_are_one_per_space_in_order = node
            .composite_clips
            .iter()
            .all(|clip| clip.raster_space_index.0 < spaces.len())
            && node
                .composite_clips
                .windows(2)
                .all(|pair| pair[0].raster_space_index < pair[1].raster_space_index);
        if !clips_are_one_per_space_in_order {
            return Err("the composite clips of a node are not one per space of the stack, in order");
        }
        let Some(effect) = node.composited_effect else {
            continue;
        };
        let effect_takes_part_in_replay = tree.effect_is_live(effect)
            && list
                .effect_clip_plan(tree)
                .output_clip_if_participating(effect)
                .is_some();
        if !effect_takes_part_in_replay {
            return Err("a node is composited inside an effect that a replay of the list does not enter");
        }
        if !effects_of_composited_nodes.insert(effect) {
            return Err("two nodes are composited inside one effect");
        }
    }
    Ok(())
}

// A frame composites a layer inside each composited effect around it, and the planner gives a list one layer where they
// nest deeper than `MAXIMUM_NESTING_OF_COMPOSITED_EFFECTS`. Each node comes after the node it is in.
fn check_nesting_of_composited_effects(nodes: &[NodeOfPlan]) -> Result<(), &'static str> {
    let mut nesting_by_node_index: Vec<usize> = Vec::with_capacity(nodes.len());
    for node in nodes {
        let nesting_around = node
            .parent_index
            .and_then(|parent_index| nesting_by_node_index.get(parent_index.0).copied())
            .unwrap_or(0);
        let nesting = nesting_around + usize::from(node.composited_effect.is_some());
        if nesting > MAXIMUM_NESTING_OF_COMPOSITED_EFFECTS {
            return Err("composited effects nest deeper than a plan nests them");
        }
        nesting_by_node_index.push(nesting);
    }
    Ok(())
}

// A frame enters the effects of the nodes around a layer outermost first, each from the one around it.
fn check_composited_effects_nest_as_in_tree(
    nodes: &[NodeOfPlan],
    nesting_of_live_effects: &NestingOfLiveEffects<'_>,
) -> Result<(), &'static str> {
    if nodes.iter().all(|node| node.composited_effect.is_none()) {
        return Ok(());
    }
    let mut innermost_effect_around_by_node_index: Vec<Option<EffectNodeIndex>> = Vec::with_capacity(nodes.len());
    for node in nodes {
        let innermost_effect_around = node.parent_index.and_then(|parent_index| {
            nodes[parent_index.0]
                .composited_effect
                .or(innermost_effect_around_by_node_index[parent_index.0])
        });
        if let (Some(effect), Some(effect_around)) = (node.composited_effect, innermost_effect_around)
            && !nesting_of_live_effects.is_strictly_inside(effect, effect_around)
        {
            return Err("the composited effects around a node are not nested as the tree nests them");
        }
        innermost_effect_around_by_node_index.push(innermost_effect_around);
    }
    Ok(())
}

// The walk of the bands enters the first band, and at each cut whose barrier is a nested band, the next band. So it
// enters each band once exactly where each band but the first has a cut before it left to enter it from, and no cut is
// left after the last band.
fn check_bands(plan: &LayeredPlan, list: &DisplayListStorage) -> Result<(), &'static str> {
    const NOT_ENTERED_ONCE: &str = "the walk of the bands does not enter each band of the stack once";
    let mut bands_left_to_enter = 1usize;
    let mut media_of_layers: Vec<(ListPosition, LiveContent, u32)> = Vec::new();
    let mut layer_count_of_live_content_drawn_by_its_record = 0usize;
    for (index, band) in plan.bands.iter().enumerate() {
        bands_left_to_enter = bands_left_to_enter.checked_sub(1).ok_or(NOT_ENTERED_ONCE)?;
        if band.placement_node_index.0 >= plan.nodes.len() {
            return Err("a band is of a node the stack does not hold");
        }
        // A plan opens each band but the first for a stretch of records that goes into that band or into a band inside
        // it. The first band holds nothing where no record can show.
        if index != 0 && band.record_spans.is_empty() && band.cuts.is_empty() {
            return Err("a band other than the first holds no span and no cut");
        }
        let spans_are_of_whole_runs_in_paint_order =
            band.record_spans.iter().all(|span| span_is_of_whole_runs(list, *span))
                && band.record_spans.windows(2).all(|pair| pair[0].end <= pair[1].begin);
        if !spans_are_of_whole_runs_in_paint_order {
            return Err("the spans of a band are not of whole runs of the list, in paint order");
        }
        for cut in &band.cuts {
            match cut.barrier {
                BarrierOfPlan::Media {
                    content,
                    ordinal,
                    record_span,
                    ..
                } => {
                    if !span_is_run_drawing_content(list, record_span, content) {
                        return Err("live content is not a run of the list that draws it");
                    }
                    if content.is_drawn_by_its_record() {
                        if layer_count_of_live_content_drawn_by_its_record
                            == MAXIMUM_LAYER_COUNT_OF_LIVE_CONTENT_DRAWN_BY_ITS_RECORD
                        {
                            return Err("the stack holds more layers of carets and scroll bars than a plan makes");
                        }
                        layer_count_of_live_content_drawn_by_its_record += 1;
                    }
                    media_of_layers.push((record_span.begin, content, ordinal));
                }
                BarrierOfPlan::NestedBand => bands_left_to_enter += 1,
            }
        }
    }
    if bands_left_to_enter != 0 {
        return Err(NOT_ENTERED_ONCE);
    }
    check_ordinals_of_media(media_of_layers)
}

// A plan numbers the drawings of live content that have layers in paint order: each has as its ordinal how many of them
// with the same content are painted before it. The window server tells the media apart by
// their ordinals.
fn check_ordinals_of_media(mut media_of_layers: Vec<(ListPosition, LiveContent, u32)>) -> Result<(), &'static str> {
    media_of_layers.sort_unstable_by_key(|(position, _, _)| *position);
    let mut drawings_painted_before_by_content: HashMap<LiveContent, u32> = HashMap::new();
    for (_, content, ordinal) in media_of_layers {
        let drawings_painted_before = drawings_painted_before_by_content.entry(content).or_insert(0);
        if ordinal != *drawings_painted_before {
            return Err("live content is not numbered by the drawings of it painted before it");
        }
        *drawings_painted_before += 1;
    }
    Ok(())
}

// A plan puts each stretch of records into one band, or for live content with a layer, into that layer, and a stretch
// holds at least one run. So no record is in two spans, and a stack holds no more spans than
// the list holds runs.
fn check_no_record_is_held_twice(plan: &LayeredPlan) -> Result<(), &'static str> {
    let spans_of_media_layers = plan
        .bands
        .iter()
        .flat_map(|band| &band.cuts)
        .filter_map(|cut| match cut.barrier {
            BarrierOfPlan::Media { record_span, .. } => Some(record_span),
            BarrierOfPlan::NestedBand => None,
        });
    let mut spans: Vec<ListSpan> = plan
        .bands
        .iter()
        .flat_map(|band| band.record_spans.iter().copied())
        .chain(spans_of_media_layers)
        .collect();
    if spans.iter().any(|span| span.begin >= span.end) {
        return Err("a span of the stack holds no record");
    }
    spans.sort_unstable_by_key(|span| span.begin);
    if spans.windows(2).any(|pair| pair[1].begin < pair[0].end) {
        return Err("two spans of the stack hold the same record");
    }
    Ok(())
}

// What only the walk of the bands tells: the node of a band the walk enters at a cut of another band is in the node of
// that band, and the first band is of a node in no other node; the cuts of a band are in paint order; and the identities
// of layers, so of bands too, are unique.
fn check_stack_of_plan(stack: &LayerStack) -> Result<(), &'static str> {
    for band in &stack.bands {
        let node_of_band_around = band
            .parent_band_holding_this_band_as_barrier
            .map(|band_around| stack.bands[band_around.0].placement_node_index);
        if node_of_band_around != stack.nodes[band.placement_node_index.0].parent_index {
            return Err("a band is not of a node in the node of the band that holds it as a barrier");
        }
        let cuts_are_in_paint_order = band
            .cuts
            .windows(2)
            .all(|pair| pair[0].barrier_first_record_position <= pair[1].barrier_first_record_position);
        if !cuts_are_in_paint_order {
            return Err("the cuts of a band are not in paint order");
        }
    }
    let mut identities_of_layers = HashSet::with_capacity(stack.layers.len());
    if !stack
        .layers
        .iter()
        .all(|layer| identities_of_layers.insert(layer.identity))
    {
        return Err("two layers share an identity");
    }
    Ok(())
}

/// The stack of a plan that another process made for `list` under `tree` within `viewport_rect`, if the plan passes the
/// checks.
///
/// The wire holds only what the plan alone knows. The rest is built here with the functions the planner calls: a stack
/// of one layer is the one `LayerStack::one_layer_replaying_whole_list` makes, and the spaces of a stack of more layers
/// are those `RasterSpaces::spaces_at_anchors` makes at the anchors of the tree, from which
/// `LayerStack::of_layered_plan` builds the rest. The layering obstacles are those of the list and the tree.
///
/// After the checks, every consumer can read the stack without a panic, an index out of range, an overflow, or work
/// without a bound. Each index names an item of the stack, the list or the tree, and each rect lies within twice the
/// extent limit of a plan. The nodes, bands, cuts and spans nest and follow each other as a plan makes them, the spans
/// are of whole runs, and no record is in two spans. So the stack holds no more spans than the list holds runs, no
/// more layers of each kind of live content than a plan makes, and its nodes nest no deeper than its spaces plus
/// `MAXIMUM_NESTING_OF_COMPOSITED_EFFECTS`. The layers of a band are next to each other, so a frame that composites
/// the layers in order enters each composited band once. The base layer is never of a space whose anchor the
/// compositor animates, and no two layers share an identity.
///
/// The checks leave out what can only make pixels wrong: whether reaches and composite clips hold what the records
/// draw, which effects must be composited, which layers can hold content, whether every record is in a span, which
/// spaces hold content that moves within them, and where the window server can show media. The list and the tree pass
/// their own checks first.
fn validated_stack(
    stack: StackOnWire,
    list: &DisplayListStorage,
    tree: &VisualContextTree,
    viewport_rect: IntRect,
) -> Result<LayerStack, &'static str> {
    let nesting_of_live_effects = NestingOfLiveEffects::of(tree);
    let layering_obstacles = LayeringObstacles::of(tree);
    match stack {
        StackOnWire::OneLayer(why_list_is_one_layer) => {
            check_viewport_rect(viewport_rect)?;
            Ok(LayerStack::one_layer_replaying_whole_list(
                viewport_rect,
                list.whole_span(),
                Some(why_list_is_one_layer),
                layering_obstacles,
            ))
        }
        StackOnWire::Layered(plan) => LayerStack::of_plan_passing_checks(
            plan,
            RasterSpaces::spaces_at_anchors(tree, &anchors_of_layer_plans(tree), viewport_rect),
            list,
            tree,
            viewport_rect,
            layering_obstacles,
            &nesting_of_live_effects,
        ),
    }
}

impl LayerStack {
    /// The stack of `plan`, a plan of more layers for `list` under `tree` within `viewport_rect` at `spaces`, once the
    /// plan passes the checks of `validated_stack`. The planner runs them on a plan before it goes to the compositor, so the
    /// compositor never refuses a plan the planner makes.
    pub(super) fn of_plan_passing_checks(
        plan: LayeredPlan,
        spaces: Vec<RasterSpace>,
        list: &DisplayListStorage,
        tree: &VisualContextTree,
        viewport_rect: IntRect,
        layering_obstacles: LayeringObstacles,
        nesting_of_live_effects: &NestingOfLiveEffects<'_>,
    ) -> Result<LayerStack, &'static str> {
        check_viewport_rect(viewport_rect)?;
        check_rects_and_coordinates(&plan)?;
        check_values_per_space(&plan, &spaces)?;
        check_nodes(&plan.nodes, &spaces, list, tree)?;
        check_nesting_of_composited_effects(&plan.nodes)?;
        check_composited_effects_nest_as_in_tree(&plan.nodes, nesting_of_live_effects)?;
        check_bands(&plan, list)?;
        check_no_record_is_held_twice(&plan)?;
        let stack = LayerStack::of_layered_plan(plan, spaces, tree, layering_obstacles);
        check_stack_of_plan(&stack)?;
        Ok(stack)
    }
}

/// Whether the compositor takes a plan made within `viewport_rect`, of one layer or more.
pub fn compositor_takes_plans_within(viewport_rect: IntRect) -> bool {
    check_viewport_rect(viewport_rect).is_ok()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::display_list::builder::DisplayListBuilder;
    use crate::display_list::commands::{ClipNodeIndex, ContextRef, SpatialNodeIndex, VISUAL_VIEWPORT_NODE_INDEX};
    use crate::display_list::layers::planning::{LayerStackInputs, PlannedLayers, plan_layer_stack};
    use crate::display_list::layers::{
        BandIndex, CutsOfCell, LARGEST_SUMMED_READ_REACH_OF_COMPOSITED_FILTERS_IN_DEVICE_PIXELS, LayerIndex,
        MAXIMUM_NESTING_OF_COMPOSITED_EFFECTS, NodeNamesFromIndices, PlacementNodeBand, WhatWindowServerShowsOfFrame,
        WhyFrameHoldsLayer, WindowServerCompositing, WindowServerPresentationOfMedia,
    };
    use crate::display_list::placement::{RasterSpace, RasterSpaceGeometry, WhatEverySpaceOfTreeShares};
    use crate::display_list::test_fixtures::{
        GREEN, ListOfOneChunk, NO_FRAME_STATE, VIEWPORT, canvas, caret, chunk_of, effects, fill, frosted_glass,
        opacity_animation_of, raster_settings_hash_by_layer, scroll_bar, scroll_node, scroll_record, spinning,
        transform, tree_with_document_scroller, tree_with_root_isolation_effect,
    };
    use crate::display_list::tile_keys::{TileKeyingOfList, TileKeyingOfRasterSpace};
    use crate::node_slot_id::NodeSlotId;
    use crate::visual_context::scroll_state::NO_SCROLL_STATE_SLOT;
    use crate::visual_context::{CompositorScrollTable, EffectNodeData, EffectsData, SpatialData, StickyData};
    use libgfx_rust::{Color, CompositingAndBlendingOperator, FloatMatrix4x4, FloatPoint, FloatRect, FloatSize};
    use std::sync::Arc;

    struct Page {
        tree: VisualContextTree,
        list: Arc<DisplayListStorage>,
    }

    impl Page {
        fn planned(&self) -> PlannedLayers {
            plan_layer_stack(&LayerStackInputs {
                list: &self.list,
                tree: &self.tree,
                scroll_offsets: &[],
                viewport_rect: VIEWPORT,
            })
        }

        fn validated(&self, stack: LayerStack) -> Result<LayerStack, &'static str> {
            validated_stack(on_wire(&stack), &self.list, &self.tree, VIEWPORT)
        }

        fn planned_plan(&self) -> LayeredPlan {
            let StackOnWire::Layered(plan) = on_wire(&self.planned().stack) else {
                panic!("the page is planned in more than one layer");
            };
            plan
        }

        fn validated_plan(&self, plan: LayeredPlan) -> Result<LayerStack, &'static str> {
            validated_stack(StackOnWire::Layered(plan), &self.list, &self.tree, VIEWPORT)
        }
    }

    // The stack as decoding its wire form gives it.
    fn on_wire(stack: &LayerStack) -> StackOnWire {
        let mut encoder = Encoder::new();
        encoder.encode(stack).expect("a planned stack encodes");
        Decoder::new(&encoder.finish().0, [])
            .decode()
            .expect("a planned stack decodes")
    }

    // A long page of fills in the document, each in a chunk of its own, under a header fixed to the viewport.
    fn plain_page() -> Page {
        let (tree, viewport, contexts) = tree_with_document_scroller(&[]);
        let document = contexts[0];
        let mut chunks = vec![chunk_of(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)];
        for line in 0..40 {
            chunks.push(chunk_of(
                fill(IntRect::new(10, 60 + line * 60, 700, 20), GREEN),
                document,
            ));
        }
        chunks.push(chunk_of(fill(IntRect::new(0, 0, 800, 50), GREEN), viewport));
        Page {
            tree,
            list: DisplayListStorage::of_chunks(chunks).unwrap(),
        }
    }

    fn page_with_two_canvases() -> Page {
        let (tree, _, contexts) = tree_with_document_scroller(&[]);
        let document = contexts[0];
        let list = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(canvas(IntRect::new(100, 100, 200, 100), 1), document)
            .with(fill(IntRect::new(150, 150, 200, 100), GREEN), document)
            .with(canvas(IntRect::new(100, 400, 200, 100), 2), document)
            .with(fill(IntRect::new(150, 450, 200, 100), GREEN), document);
        Page {
            tree,
            list: list.storage(),
        }
    }

    fn page_drawing_one_canvas_twice() -> Page {
        let (tree, _, contexts) = tree_with_document_scroller(&[]);
        let document = contexts[0];
        let list = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(canvas(IntRect::new(100, 100, 200, 100), 1), document)
            .with(fill(IntRect::new(150, 150, 200, 100), GREEN), document)
            .with(canvas(IntRect::new(100, 400, 200, 100), 1), document);
        Page {
            tree,
            list: list.storage(),
        }
    }

    fn page_with_canvas_in_nested_scroller() -> Page {
        let (tree, _, contexts) = tree_with_document_scroller(&[IntRect::new(100, 100, 300, 200)]);
        let (document, nested) = (contexts[0], contexts[1]);
        let list = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(fill(IntRect::new(100, 100, 300, 700), GREEN), nested)
            .with(canvas(IntRect::new(120, 150, 100, 100), 1), nested)
            .with(fill(IntRect::new(150, 200, 100, 100), GREEN), nested)
            .with(fill(IntRect::new(0, 900, 800, 100), GREEN), document);
        Page {
            tree,
            list: list.storage(),
        }
    }

    fn page_with_sticky_header() -> Page {
        let (mut tree, _, contexts) = tree_with_document_scroller(&[]);
        let document = contexts[0];
        let mut sticky_data =
            StickyData::unconstrained(document.spatial, None, NO_SCROLL_STATE_SLOT, NodeSlotId::INVALID);
        sticky_data.position_relative_to_scroller = FloatPoint { x: 0.0, y: 100.0 };
        sticky_data.border_box_size = FloatSize {
            width: 800.0,
            height: 50.0,
        };
        sticky_data.scrollport_size = FloatSize {
            width: 800.0,
            height: 600.0,
        };
        sticky_data.containing_block_region = FloatRect::new(0.0, 0.0, 800.0, 2000.0);
        sticky_data.needs_parent_offset_adjustment = true;
        sticky_data.inset_top = Some(0.0);
        let sticky_node = tree.append_spatial(SpatialData::Sticky(sticky_data), document.spatial);
        let sticky = ContextRef {
            spatial: sticky_node,
            ..document
        };
        let list = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(fill(IntRect::new(0, 100, 800, 50), GREEN), sticky)
            .with(fill(IntRect::new(0, 300, 800, 50), GREEN), document);
        Page {
            tree,
            list: list.storage(),
        }
    }

    // An element whose opacity the compositor animates holds another whose opacity it animates.
    fn page_with_nested_composited_effects() -> Page {
        let (mut tree, _, contexts) = tree_with_document_scroller(&[]);
        let document = contexts[0];
        let outer = tree.append_effect_node_under(document, effects(1.0, CompositingAndBlendingOperator::Normal));
        let inner = tree.append_effect_node_under(outer, effects(1.0, CompositingAndBlendingOperator::Normal));
        tree.set_visual_animations(vec![
            opacity_animation_of(outer.effect, 1.0, 0.0),
            opacity_animation_of(inner.effect, 1.0, 0.0),
        ]);
        let list = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(fill(IntRect::new(100, 100, 300, 300), GREEN), outer)
            .with(fill(IntRect::new(150, 150, 100, 100), GREEN), inner)
            .with(fill(IntRect::new(0, 500, 800, 50), GREEN), document);
        Page {
            tree,
            list: list.storage(),
        }
    }

    fn page_with_spinner() -> Page {
        let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
        let spinner_node = tree.append_spatial(
            SpatialData::Transform(transform(FloatRect::new(50.0, 50.0, 40.0, 40.0))),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        tree.set_visual_animations(vec![spinning(spinner_node)]);
        let spinner = ContextRef {
            spatial: spinner_node,
            ..viewport
        };
        let list = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 100, 100), GREEN), viewport)
            .with(fill(IntRect::new(50, 50, 40, 40), GREEN), spinner)
            .with(fill(IntRect::new(0, 100, 100, 100), GREEN), viewport);
        Page {
            tree,
            list: list.storage(),
        }
    }

    // The background is painted below the root group, which a spinner holds all of, so the compositor composites the
    // root isolation effect over the spinner's transform.
    fn page_with_spinner_holding_the_root_group() -> Page {
        let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
        let spinner_node = tree.append_spatial(
            SpatialData::Transform(transform(FloatRect::new(50.0, 50.0, 40.0, 40.0))),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        tree.set_visual_animations(vec![spinning(spinner_node)]);
        let spinner = ContextRef {
            spatial: spinner_node,
            ..viewport
        };
        let list = ListOfOneChunk::default()
            .with(fill(VIEWPORT, GREEN), ContextRef::default())
            .with(fill(IntRect::new(50, 50, 40, 40), GREEN), spinner);
        Page {
            tree,
            list: list.storage(),
        }
    }

    fn page_under_translucent_root() -> Page {
        let (tree, viewport) = tree_with_root_isolation_effect(0.5);
        let list = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 600), GREEN), viewport)
            .with(canvas(IntRect::new(100, 100, 200, 100), 1), viewport);
        Page {
            tree,
            list: list.storage(),
        }
    }

    fn page_without_records() -> Page {
        let (tree, _) = tree_with_root_isolation_effect(1.0);
        Page {
            tree,
            list: DisplayListStorage::of_chunks(Vec::new()).unwrap(),
        }
    }

    // A record that draws nothing, which no band holds, so that the only band of the stack holds nothing.
    fn page_whose_records_draw_nothing() -> Page {
        let (tree, viewport) = tree_with_root_isolation_effect(1.0);
        let list = ListOfOneChunk::default().with(fill(IntRect::new(10, 10, 0, 0), GREEN), viewport);
        Page {
            tree,
            list: list.storage(),
        }
    }

    // More scrollers in the document than get spaces.
    const NESTED_SCROLLER_COUNT_OF_CROWDED_PAGE: i32 = 80;

    fn page_with_many_nested_scrollers() -> Page {
        let scrollport_rects: Vec<IntRect> = (0..NESTED_SCROLLER_COUNT_OF_CROWDED_PAGE)
            .map(|index| IntRect::new(10, 100 + index * 220, 300, 200))
            .collect();
        let (tree, _, contexts) = tree_with_document_scroller(&scrollport_rects);
        let mut list = ListOfOneChunk::default().with(fill(IntRect::new(0, 0, 800, 20000), GREEN), contexts[0]);
        for (scrollport_rect, context) in scrollport_rects.iter().zip(&contexts[1..]) {
            list = list.with(fill(*scrollport_rect, GREEN), *context);
        }
        Page {
            tree,
            list: list.storage(),
        }
    }

    // One canvas more than get layers, the last after a fill.
    fn page_with_nine_canvases() -> Page {
        let (tree, viewport) = tree_with_root_isolation_effect(1.0);
        let mut list = ListOfOneChunk::default().with(fill(VIEWPORT, GREEN), viewport);
        for index in 0..9 {
            list = list
                .with(fill(IntRect::new(10 + index * 80, 10, 60, 60), GREEN), viewport)
                .with(
                    canvas(IntRect::new(10 + index * 80, 100, 60, 60), index as u64 + 1),
                    viewport,
                );
        }
        Page {
            tree,
            list: list.storage(),
        }
    }

    // One scroller with one scroll bar more than get layers, painted last.
    fn page_with_one_scroll_bar_past_the_most_that_get_layers() -> Page {
        let (tree, _, contexts) = tree_with_document_scroller(&[IntRect::new(100, 100, 300, 200)]);
        let (document, scroller) = (contexts[0], contexts[1]);
        let mut list = ListOfOneChunk::default().with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document);
        for index in 0..=MAXIMUM_LAYER_COUNT_OF_LIVE_CONTENT_DRAWN_BY_ITS_RECORD as i32 {
            list = list.with(
                scroll_bar(scroller.spatial, IntRect::new(10 + index * 12, 100, 10, 200)),
                document,
            );
        }
        Page {
            tree,
            list: list.storage(),
        }
    }

    // Two elements side by side whose opacity the compositor animates.
    fn page_with_two_fading_siblings() -> Page {
        let (mut tree, _, contexts) = tree_with_document_scroller(&[]);
        let document = contexts[0];
        let left = tree.append_effect_node_under(document, effects(1.0, CompositingAndBlendingOperator::Normal));
        let right = tree.append_effect_node_under(document, effects(1.0, CompositingAndBlendingOperator::Normal));
        tree.set_visual_animations(vec![
            opacity_animation_of(left.effect, 1.0, 0.0),
            opacity_animation_of(right.effect, 1.0, 0.0),
        ]);
        let list = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(fill(IntRect::new(100, 100, 200, 200), GREEN), left)
            .with(fill(IntRect::new(400, 100, 200, 200), GREEN), right)
            .with(fill(IntRect::new(0, 500, 800, 50), GREEN), document);
        Page {
            tree,
            list: list.storage(),
        }
    }

    // A backdrop filter over the document alone, which reads only its own layer, and one over an earlier canvas, which
    // the compositor composites.
    fn page_with_frosted_glass() -> Page {
        let (mut tree, _, contexts) = tree_with_document_scroller(&[]);
        let document = contexts[0];
        let over_document = frosted_glass(&mut tree, document, IntRect::new(400, 100, 200, 100));
        let over_canvas = frosted_glass(&mut tree, document, IntRect::new(100, 100, 200, 100));
        let list = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(fill(IntRect::new(400, 100, 200, 100), GREEN), over_document)
            .with(canvas(IntRect::new(100, 100, 200, 100), 1), document)
            .with(fill(IntRect::new(100, 100, 200, 100), GREEN), over_canvas)
            .with(fill(IntRect::new(0, 500, 800, 50), GREEN), document);
        Page {
            tree,
            list: list.storage(),
        }
    }

    // Two carets, the first under a box painted over it.
    fn page_with_carets() -> Page {
        let (tree, _, contexts) = tree_with_document_scroller(&[]);
        let document = contexts[0];
        let list = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(caret(IntRect::new(20, 420, 2, 16)), document)
            .with(caret(IntRect::new(20, 1420, 2, 16)), document)
            .with(fill(IntRect::new(0, 400, 800, 50), GREEN), document);
        Page {
            tree,
            list: list.storage(),
        }
    }

    // A nested scroller with its vertical scroll bar.
    fn page_with_scroll_bar() -> Page {
        let (tree, _, contexts) = tree_with_document_scroller(&[IntRect::new(100, 100, 300, 200)]);
        let (document, nested) = (contexts[0], contexts[1]);
        let list = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(fill(IntRect::new(100, 100, 300, 700), GREEN), nested)
            .with(scroll_bar(nested.spatial, IntRect::new(390, 100, 10, 200)), document);
        Page {
            tree,
            list: list.storage(),
        }
    }

    // A spinner in a group scaled up eighty times, whose space a plan rasters at that scale.
    fn page_with_spinner_in_large_scale() -> Page {
        let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
        let mut scaling = transform(FloatRect::new(0.0, 0.0, 10.0, 10.0));
        scaling.matrix = libgfx_rust::scale_matrix(80.0, 80.0, 1.0);
        let scaled_node = tree.append_spatial(SpatialData::Transform(scaling), VISUAL_VIEWPORT_NODE_INDEX);
        let spinner_node = tree.append_spatial(
            SpatialData::Transform(transform(FloatRect::new(1.0, 1.0, 4.0, 4.0))),
            scaled_node,
        );
        tree.set_visual_animations(vec![spinning(spinner_node)]);
        let spinner = ContextRef {
            spatial: spinner_node,
            ..viewport
        };
        let list = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 100, 100), GREEN), viewport)
            .with(fill(IntRect::new(1, 1, 4, 4), GREEN), spinner)
            .with(fill(IntRect::new(0, 100, 100, 100), GREEN), viewport);
        Page {
            tree,
            list: list.storage(),
        }
    }

    // Translucent groups nested in each other, with a canvas in the group `canvas_depth` deep, so that the compositor
    // composites the groups around it, and a fill in the innermost group.
    fn page_with_canvas_in_nested_groups(group_count: usize, canvas_depth: usize) -> Page {
        let (mut tree, _, contexts) = tree_with_document_scroller(&[]);
        let document = contexts[0];
        let mut groups = vec![document];
        for _ in 0..group_count {
            groups.push(tree.append_effect_node_under(
                groups[groups.len() - 1],
                effects(0.5, CompositingAndBlendingOperator::Normal),
            ));
        }
        let list = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 800, 2600), GREEN), document)
            .with(canvas(IntRect::new(100, 100, 200, 100), 1), groups[canvas_depth])
            .with(fill(IntRect::new(150, 150, 200, 100), GREEN), groups[group_count])
            .with(fill(IntRect::new(0, 500, 800, 50), GREEN), document);
        Page {
            tree,
            list: list.storage(),
        }
    }

    fn every_page() -> Vec<(&'static str, Page)> {
        vec![
            ("plain page", plain_page()),
            ("two canvases", page_with_two_canvases()),
            ("canvas in nested scroller", page_with_canvas_in_nested_scroller()),
            ("sticky header", page_with_sticky_header()),
            ("nested composited effects", page_with_nested_composited_effects()),
            ("spinner", page_with_spinner()),
            (
                "spinner holding the root group",
                page_with_spinner_holding_the_root_group(),
            ),
            ("translucent root", page_under_translucent_root()),
            ("no records", page_without_records()),
            ("records that draw nothing", page_whose_records_draw_nothing()),
            ("frosted glass", page_with_frosted_glass()),
            ("carets", page_with_carets()),
            ("scroll bar", page_with_scroll_bar()),
        ]
    }

    // The plan WebContent sends of what it planned.
    fn plan_sent_of(planned: &PlannedLayers) -> LayerPlan {
        LayerPlan {
            display_list_id: 42,
            viewport_rect: VIEWPORT,
            stack: planned.stack.clone(),
            note_lines: planned.note_lines_for_layers_overlay(),
        }
    }

    fn encoded(plan: &LayerPlan) -> Vec<u8> {
        let mut encoder = Encoder::new();
        encoder.encode(plan).expect("a planned stack encodes");
        encoder.finish().0
    }

    fn decoded_and_validated_plan(page: &Page, bytes: &[u8]) -> Result<LayerPlan, &'static str> {
        let mut decoder = Decoder::new(bytes, []);
        let plan: UnvalidatedLayerPlan = decoder.decode().map_err(|_| "the plan does not decode")?;
        if !decoder.is_at_end() {
            return Err("bytes follow the plan");
        }
        plan.validated(&page.list, &page.tree)
    }

    fn kinds_of_anchors(stack: &LayerStack) -> Vec<&'static str> {
        stack
            .spaces
            .iter()
            .map(|space| match space.anchor_kind {
                RasterSpaceAnchorKind::Viewport => "viewport",
                RasterSpaceAnchorKind::ViewportScroller => "document",
                RasterSpaceAnchorKind::Scroller => "scroller",
                RasterSpaceAnchorKind::StickyBox { .. } => "sticky",
                RasterSpaceAnchorKind::AnimatedTransform { .. } => "animated",
            })
            .collect()
    }

    fn media_layer_count(stack: &LayerStack) -> usize {
        stack
            .layers
            .iter()
            .filter(|layer| matches!(layer.content, LayerContent::Media { .. }))
            .count()
    }

    #[test]
    fn the_pages_plan_the_stacks_the_tests_are_about() {
        let stack_of = |page: Page| page.planned().stack;
        let plain = stack_of(plain_page());
        assert_eq!(kinds_of_anchors(&plain), ["viewport", "document"]);
        assert!(plain.why_list_is_one_layer.is_none());
        assert_eq!(media_layer_count(&stack_of(page_with_two_canvases())), 2);
        let media_identities_of_one_canvas_drawn_twice: Vec<LayerIdentity> = stack_of(page_drawing_one_canvas_twice())
            .layers
            .iter()
            .filter(|layer| matches!(layer.content, LayerContent::Media { .. }))
            .map(|layer| layer.identity)
            .collect();
        let drawing = |ordinal| LayerIdentity::Media {
            content: LiveContent::Canvas(CanvasId(1)),
            ordinal,
        };
        assert_eq!(media_identities_of_one_canvas_drawn_twice, [drawing(0), drawing(1)]);
        let nested = stack_of(page_with_canvas_in_nested_scroller());
        assert_eq!(kinds_of_anchors(&nested), ["viewport", "document", "scroller"]);
        assert_eq!(media_layer_count(&nested), 1);
        assert_eq!(
            kinds_of_anchors(&stack_of(page_with_sticky_header())),
            ["viewport", "document", "sticky"]
        );
        let composited = stack_of(page_with_nested_composited_effects());
        assert_eq!(composited.composited_effects.len(), 2);
        assert_eq!(
            kinds_of_anchors(&stack_of(page_with_spinner())),
            ["viewport", "animated"]
        );
        let root_page = page_with_spinner_holding_the_root_group();
        let root_effect = root_page.tree.root_isolation_effect;
        let composited_effects_of_root_page: Vec<EffectNodeIndex> = stack_of(root_page)
            .composited_effects
            .iter()
            .map(|composited| composited.effect)
            .collect();
        assert_eq!(composited_effects_of_root_page, Vec::from_iter(root_effect));
        assert_eq!(
            stack_of(page_under_translucent_root()).why_list_is_one_layer,
            Some(WhyListIsOneLayer::RootIsolationEffectIsNotPlainGroup)
        );
        assert_eq!(
            stack_of(page_without_records()).why_list_is_one_layer,
            Some(WhyListIsOneLayer::EmptyDisplayList)
        );
        let nothing_drawn = stack_of(page_whose_records_draw_nothing());
        assert!(nothing_drawn.why_list_is_one_layer.is_none());
        assert_eq!(nothing_drawn.bands.len(), 1);
        assert!(nothing_drawn.bands[0].record_spans.is_empty() && nothing_drawn.bands[0].cuts.is_empty());
        let carets = stack_of(page_with_carets());
        let caret_layers: Vec<(LayerIdentity, bool)> = carets
            .bands
            .iter()
            .flat_map(|band| &band.cuts)
            .map(|cut| {
                (
                    carets.layers[cut.layer_above_cut.0 - 1].identity,
                    cut.cut_reach != Reach::Nothing,
                )
            })
            .filter(|(identity, _)| matches!(identity, LayerIdentity::Media { .. }))
            .collect();
        let caret = |ordinal| LayerIdentity::Media {
            content: LiveContent::Caret,
            ordinal,
        };
        assert_eq!(caret_layers, [(caret(0), true), (caret(1), false)]);
        let scroll_bar_layers: Vec<LayerIdentity> = stack_of(page_with_scroll_bar())
            .layers
            .iter()
            .filter(|layer| matches!(layer.content, LayerContent::OwnSurface { .. }))
            .map(|layer| layer.identity)
            .collect();
        assert!(matches!(
            scroll_bar_layers[..],
            [LayerIdentity::Media {
                content: LiveContent::ScrollBar { vertical: true, .. },
                ordinal: 0
            }]
        ));
        let crowded = stack_of(page_with_many_nested_scrollers());
        assert_eq!(kinds_of_anchors(&crowded)[2..], ["scroller"; 6]);
        let nested_scrollers_without_spaces = NESTED_SCROLLER_COUNT_OF_CROWDED_PAGE as usize - 6;
        assert!(crowded.spaces.len() + nested_scrollers_without_spaces > MAXIMUM_RASTER_SPACE_COUNT);
        let nine_canvases = stack_of(page_with_nine_canvases());
        assert_eq!(nine_canvases.bands.len(), 1);
        assert_eq!(media_layer_count(&nine_canvases), 9);
        let siblings = stack_of(page_with_two_fading_siblings());
        let parents_of_composited_nodes: Vec<Option<PlacementNodeIndex>> = siblings
            .composited_effects
            .iter()
            .map(|composited| siblings.nodes[composited.placement_node_index.0].parent_index)
            .collect();
        assert_eq!(parents_of_composited_nodes.len(), 2);
        assert_eq!(parents_of_composited_nodes[0], parents_of_composited_nodes[1]);
        let frosted = stack_of(page_with_frosted_glass());
        let reads_of_composited_effects: Vec<bool> = frosted
            .composited_effects
            .iter()
            .map(|composited| composited.reads_what_is_composited_below_it)
            .collect();
        assert_eq!(reads_of_composited_effects, [true]);
        // The spinner is rastered at the scale of its group, far past four times the scale of the viewport.
        let large_scale = stack_of(page_with_spinner_in_large_scale());
        assert!(matches!(
            large_scale.spaces[1..],
            [RasterSpace {
                anchor_kind: RasterSpaceAnchorKind::AnimatedTransform { raster_scale },
                ..
            }] if raster_scale > 64.0
        ));
    }

    #[test]
    fn a_planned_stack_round_trips_through_its_wire_form_and_its_checks() {
        let pages = every_page()
            .into_iter()
            .chain([("spinner in large scale", page_with_spinner_in_large_scale())]);
        for (name, page) in pages {
            let sent = plan_sent_of(&page.planned());
            let plan = decoded_and_validated_plan(&page, &encoded(&sent))
                .unwrap_or_else(|why| panic!("the plan of the {name} is rejected: {why}"));
            assert_eq!(plan, sent, "the plan of the {name} comes back otherwise");
        }
    }

    #[test]
    fn the_compositor_works_out_what_a_plan_on_the_wire_leaves_out() {
        for page in [
            page_with_sticky_header(),
            page_with_frosted_glass(),
            page_under_translucent_root(),
        ] {
            let planned = page.planned();
            let mut stack = planned.stack.clone();
            for space in &mut stack.spaces {
                space.identity ^= 1;
                space.extent = IntRect::new(1, 2, 3, 4);
            }
            for composited in &mut stack.composited_effects {
                composited.reads_what_is_composited_below_it ^= true;
                composited.filter_spreads_what_it_composites ^= true;
            }
            stack.layering_obstacles.sorts_planes_by_depth ^= true;
            assert_eq!(page.validated(stack), Ok(planned.stack));
        }
    }

    // Boxes of the document, each 1 px, alternating with boxes fixed to the viewport that overlap them, so that each fixed
    // box closes the band of the document's space and the next box opens another, cut into the band of the viewport.
    fn page_of_document_boxes_alternating_with_fixed_boxes(pair_count: usize) -> Page {
        let (tree, viewport, contexts) = tree_with_document_scroller(&[]);
        let document = contexts[0];
        let mut builder = DisplayListBuilder::new();
        for _ in 0..pair_count {
            builder.append(&fill(IntRect::new(0, 0, 1, 1), GREEN), &[], document);
            builder.append(&fill(IntRect::new(0, 0, 1, 1), GREEN), &[], viewport);
        }
        Page {
            tree,
            list: DisplayListStorage::of_builder(&builder),
        }
    }

    #[test]
    fn a_plan_holding_more_than_the_compositor_decodes_is_one_layer() {
        let page_at_limit = page_of_document_boxes_alternating_with_fixed_boxes(MAXIMUM_CUT_COUNT_OF_RECEIVED_BAND);
        let planned_at_limit = page_at_limit.planned();
        assert_eq!(planned_at_limit.stack.why_list_is_one_layer, None);
        assert_eq!(
            planned_at_limit.stack.bands.iter().map(|band| band.cuts.len()).max(),
            Some(MAXIMUM_CUT_COUNT_OF_RECEIVED_BAND)
        );
        let sent_at_limit = plan_sent_of(&planned_at_limit);
        assert_eq!(
            decoded_and_validated_plan(&page_at_limit, &encoded(&sent_at_limit)),
            Ok(sent_at_limit)
        );

        let page = page_of_document_boxes_alternating_with_fixed_boxes(MAXIMUM_CUT_COUNT_OF_RECEIVED_BAND + 1);
        let planned = page.planned();
        assert_eq!(
            planned.stack.why_list_is_one_layer,
            Some(WhyListIsOneLayer::PlanHoldsMoreThanCompositorDecodes)
        );
        let sent = plan_sent_of(&planned);
        assert_eq!(decoded_and_validated_plan(&page, &encoded(&sent)), Ok(sent));
    }

    // A page can nest composited effects deeper than a walk over the nodes of its plan can hold frames of on a stack.
    #[test]
    fn composited_effects_nested_far_past_the_limit_give_one_layer() {
        let page = page_with_canvas_in_nested_groups(100_000, 100_000);
        assert_eq!(
            page.planned().stack.why_list_is_one_layer,
            Some(WhyListIsOneLayer::CompositedEffectsNestTooDeep)
        );
    }

    #[test]
    fn composited_effects_nest_in_a_plan_as_deep_as_the_limit() {
        let deepest = MAXIMUM_NESTING_OF_COMPOSITED_EFFECTS;
        let page_at_limit = page_with_canvas_in_nested_groups(deepest, deepest);
        let planned = page_at_limit.planned();
        let stack = &planned.stack;
        assert_eq!(stack.why_list_is_one_layer, None);
        let (canvas_layer_index, _) = stack
            .layers
            .iter()
            .enumerate()
            .find(|(_, layer)| matches!(layer.content, LayerContent::Media { .. }))
            .expect("the canvas has a layer");
        assert_eq!(
            stack
                .composited_bands_around_layer_innermost_first(LayerIndex(canvas_layer_index))
                .count(),
            deepest
        );
        let sent = plan_sent_of(&planned);
        assert_eq!(decoded_and_validated_plan(&page_at_limit, &encoded(&sent)), Ok(sent));

        let page_past_limit = page_with_canvas_in_nested_groups(deepest + 1, deepest + 1);
        assert_eq!(
            page_past_limit.planned().stack.why_list_is_one_layer,
            Some(WhyListIsOneLayer::CompositedEffectsNestTooDeep)
        );

        // The innermost group holds only a fill, so it is not composited, until a plan says it is.
        let page_with_group_past_limit = page_with_canvas_in_nested_groups(deepest + 1, deepest);
        let mut plan = page_with_group_past_limit.planned_plan();
        assert!(page_with_group_past_limit.validated_plan(plan.clone()).is_ok());
        let innermost_group = EffectNodeIndex(page_with_group_past_limit.tree.effect_nodes.len() as u32 - 1);
        let group_around = EffectNodeIndex(innermost_group.0 - 1);
        let node_of_group_around = plan
            .nodes
            .iter()
            .position(|node| node.composited_effect == Some(group_around))
            .expect("the group around the canvas is composited");
        plan.nodes.push(NodeOfPlan {
            composited_effect: Some(innermost_group),
            parent_index: Some(PlacementNodeIndex(node_of_group_around)),
            reach_in_parent_node: Reach::Nothing,
            ..plan.nodes[node_of_group_around].clone()
        });
        assert_eq!(
            page_with_group_past_limit.validated_plan(plan),
            Err("composited effects nest deeper than a plan nests them")
        );
    }

    #[test]
    fn the_read_reaches_of_the_composited_filters_around_a_layer_add_up_to_at_most_the_largest_sum() {
        let stack = page_with_nested_composited_effects().planned().stack;
        let read_reach_of_one_filter = 5000;
        let read_reach_by_layer =
            stack.summed_read_reach_of_composited_filters_by_layer(|_, _| read_reach_of_one_filter);
        let largest = LARGEST_SUMMED_READ_REACH_OF_COMPOSITED_FILTERS_IN_DEVICE_PIXELS;
        assert!(read_reach_of_one_filter < largest && 2 * read_reach_of_one_filter > largest);
        let expected: Vec<i32> = (0..stack.layers.len())
            .map(|layer_index| {
                let nesting = stack
                    .composited_bands_around_layer_innermost_first(LayerIndex(layer_index))
                    .count() as i32;
                (nesting * read_reach_of_one_filter).min(largest)
            })
            .collect();
        assert_eq!(read_reach_by_layer, expected);
        assert!(read_reach_by_layer.contains(&read_reach_of_one_filter));
        assert!(read_reach_by_layer.contains(&largest));
    }

    #[test]
    fn a_plan_for_a_viewport_past_the_coordinate_limit_is_refused() {
        let viewport_past_limit = IntRect::new(0, 0, COORDINATE_LIMIT_IN_DEVICE_PIXELS + 1, 600);
        for page in [plain_page(), page_under_translucent_root()] {
            let stack = page.planned().stack;
            assert!(validated_stack(on_wire(&stack), &page.list, &page.tree, VIEWPORT).is_ok());
            assert!(validated_stack(on_wire(&stack), &page.list, &page.tree, viewport_past_limit).is_err());
        }
    }

    // The overlay of layers showed these lines of the plans of these pages before the plan carried them as text.
    #[test]
    fn the_note_lines_of_a_plan_are_the_lines_the_overlay_of_layers_showed() {
        let lines_of = |page: Page| page.planned().note_lines_for_layers_overlay();
        assert_eq!(
            lines_of(page_with_nested_composited_effects()),
            [
                "composited e1 space=document: the compositor animates it",
                "composited e2 space=document inside e1: the compositor animates it",
            ]
        );
        assert_eq!(
            lines_of(page_with_spinner_holding_the_root_group()),
            ["composited e0 space=animated@s1 in viewport: it applies over a transform the compositor animates"]
        );
        assert!(lines_of(page_with_nine_canvases()).is_empty());
        assert_eq!(
            lines_of(page_with_one_scroll_bar_past_the_most_that_get_layers()),
            ["no layer for vertical scroll bar: past the most media that get layers"]
        );
        assert_eq!(
            lines_of(page_with_two_fading_siblings()),
            [
                "composited e1 space=document: the compositor animates it",
                "composited e2 space=document: the compositor animates it",
            ]
        );
        assert_eq!(
            lines_of(page_with_frosted_glass()),
            [
                "composited e2 space=document reads=[73,73 254x154]: it reads what its own layer does not hold",
                "e1 is not composited: it reads only its own layer",
            ]
        );
        assert!(lines_of(page_under_translucent_root()).is_empty());
        assert!(lines_of(plain_page()).is_empty());
    }

    #[test]
    fn note_lines_past_what_a_plan_holds_are_cut() {
        // The last character that fits is cut in two at the length a line holds, so the line ends before it.
        let long_line = format!("a{}", "é".repeat(MAXIMUM_NOTE_LINE_LENGTH_OF_PLAN));
        let lines = note_lines_a_plan_holds(std::iter::repeat_n(long_line, MAXIMUM_NOTE_LINE_COUNT_OF_PLAN + 1));
        assert_eq!(lines.len(), MAXIMUM_NOTE_LINE_COUNT_OF_PLAN);
        assert!(
            lines
                .iter()
                .all(|line| line.len() == MAXIMUM_NOTE_LINE_LENGTH_OF_PLAN - 1)
        );
        let page = plain_page();
        let sent = LayerPlan {
            note_lines: lines,
            ..plan_sent_of(&page.planned())
        };
        assert_eq!(decoded_and_validated_plan(&page, &encoded(&sent)), Ok(sent));
    }

    #[test]
    fn every_reason_and_kind_round_trips() {
        fn round_trip<T: Encode + Decode + PartialEq + std::fmt::Debug>(value: T) {
            let mut encoder = Encoder::new();
            encoder.encode(&value).unwrap();
            let bytes = encoder.finish().0;
            let mut decoder = Decoder::new(&bytes, []);
            assert_eq!(decoder.decode::<T>().unwrap(), value);
            assert!(decoder.is_at_end());
        }
        for why in [
            WhyCompositorCannotCompositeEffect::FilterMovesWhatItFilters,
            WhyCompositorCannotCompositeEffect::FilterReadsFartherThanTilesAreFollowed,
        ] {
            round_trip(why);
            round_trip(WhyListIsOneLayer::EffectTheCompositorCannotCompositeIsComposited(why));
        }
        for why in [
            WhyListIsOneLayer::EmptyViewport,
            WhyListIsOneLayer::EmptyDisplayList,
            WhyListIsOneLayer::SortingContext3D,
            WhyListIsOneLayer::RootIsolationEffectIsNotPlainGroup,
            WhyListIsOneLayer::RecordsBlendWithWhatOtherLayersHoldBelowThem,
            WhyListIsOneLayer::CompositedEffectsNestTooDeep,
            WhyListIsOneLayer::FrameIsNotDrawnAsPlanned,
            WhyListIsOneLayer::PlanShowsTooManyTiles,
            WhyListIsOneLayer::PlanFailsChecksOfCompositor,
        ] {
            round_trip(why);
        }
        for content in [
            LiveContent::Canvas(CanvasId(1)),
            LiveContent::VideoFrame(VideoSinkHandle(2)),
            LiveContent::ImageFrame(ImageFrameResourceId(3)),
            LiveContent::CompositedContext(CompositorContextId(4)),
        ] {
            round_trip(content);
        }
        for reach in [Reach::Nothing, Reach::Within(IntRect::new(1, 2, 3, 4)), Reach::Anywhere] {
            round_trip(reach);
        }
    }

    #[test]
    fn a_count_or_a_length_past_its_maximum_is_refused_before_anything_is_allocated() {
        let mut encoder = Encoder::new();
        encoder.encode(&PLAN_KIND_LAYERED).unwrap();
        encoder.encode_size(MAXIMUM_NODE_COUNT_OF_RECEIVED_STACK + 1).unwrap();
        let bytes = encoder.finish().0;
        assert!(matches!(
            Decoder::new(&bytes, []).decode::<StackOnWire>(),
            Err(DecodeError::SizeTooLarge(_))
        ));
        let page = page_with_two_canvases();
        let plan_with_note_lines = |note_lines: Vec<String>| {
            encoded(&LayerPlan {
                note_lines,
                ..plan_sent_of(&page.planned())
            })
        };
        let too_many_lines = vec![String::new(); MAXIMUM_NOTE_LINE_COUNT_OF_PLAN + 1];
        let too_long_line = vec!["x".repeat(MAXIMUM_NOTE_LINE_LENGTH_OF_PLAN + 1)];
        for bytes in [
            plan_with_note_lines(too_many_lines),
            plan_with_note_lines(too_long_line),
        ] {
            assert!(matches!(
                Decoder::new(&bytes, []).decode::<UnvalidatedLayerPlan>(),
                Err(DecodeError::SizeTooLarge(_))
            ));
        }
        // A count of many spans that the bytes cannot hold is refused too.
        let mut bytes = encoded(&plan_sent_of(&page.planned()));
        bytes.truncate(bytes.len() / 2);
        assert!(decoded_and_validated_plan(&page, &bytes).is_err());
    }

    // The band and the cut of the first canvas, video or nested context of the plan.
    fn first_media_cut(plan: &LayeredPlan) -> (usize, usize) {
        plan.bands
            .iter()
            .enumerate()
            .find_map(|(band_index, band)| {
                band.cuts
                    .iter()
                    .position(|cut| matches!(cut.barrier, BarrierOfPlan::Media { .. }))
                    .map(|cut_index| (band_index, cut_index))
            })
            .expect("the plan has a canvas")
    }

    fn first_media_of(plan: &mut LayeredPlan) -> (&mut LiveContent, &mut u32, &mut ListSpan, &mut IntRect) {
        let (band_index, cut_index) = first_media_cut(plan);
        let BarrierOfPlan::Media {
            content,
            ordinal,
            record_span,
            reach,
            ..
        } = &mut plan.bands[band_index].cuts[cut_index].barrier
        else {
            unreachable!()
        };
        (content, ordinal, record_span, reach)
    }

    fn band_with_cuts(plan: &LayeredPlan) -> usize {
        plan.bands
            .iter()
            .position(|band| band.cuts.len() >= 2)
            .expect("the plan has a band with two cuts")
    }

    fn band_with_spans(plan: &LayeredPlan) -> usize {
        plan.bands
            .iter()
            .position(|band| !band.record_spans.is_empty())
            .expect("the plan has a band with spans")
    }

    fn composited_node_indices(plan: &LayeredPlan) -> Vec<usize> {
        (0..plan.nodes.len())
            .filter(|&index| plan.nodes[index].composited_effect.is_some())
            .collect()
    }

    fn band_of_node(plan: &LayeredPlan, node: usize) -> usize {
        plan.bands
            .iter()
            .position(|band| band.placement_node_index.0 == node)
            .expect("the node has a band")
    }

    // Gives the last record of the band of the first live content with a layer a layer as a plan gives one to that
    // content, numbered after the drawings of it that the band already gives layers.
    fn give_last_record_of_band_of_first_media_a_layer(plan: &mut LayeredPlan) {
        let (band_index, cut_index) = first_media_cut(plan);
        let BarrierOfPlan::Media {
            content,
            record_span: span_of_first_media,
            reach,
            ..
        } = plan.bands[band_index].cuts[cut_index].barrier
        else {
            unreachable!()
        };
        let band = &mut plan.bands[band_index];
        let drawings_of_content_with_layers = band
            .cuts
            .iter()
            .filter(|cut| matches!(cut.barrier, BarrierOfPlan::Media { content: drawn, .. } if drawn == content))
            .count() as u32;
        let span_holding_last_record = band.record_spans.last_mut().expect("the band holds records");
        let record_size = span_of_first_media.end.offset - span_of_first_media.begin.offset;
        let span_of_last_record = ListSpan {
            begin: ListPosition::new(
                span_holding_last_record.end.chunk_index,
                span_holding_last_record.end.offset - record_size,
            ),
            end: span_holding_last_record.end,
        };
        span_holding_last_record.end = span_of_last_record.begin;
        band.cuts.push(CutOfPlan {
            barrier: BarrierOfPlan::Media {
                content,
                ordinal: drawings_of_content_with_layers,
                record_span: span_of_last_record,
                reach,
                cuts_band: true,
            },
            reach_of_layer_above_cut: IntRect::default(),
        });
    }

    // Moves the records of the band of the fixed header of the plain page into a band of a node of the viewport's space
    // that no cut enters.
    fn move_header_into_band_no_cut_enters(plan: &mut LayeredPlan) {
        let header_span = plan.bands[0]
            .record_spans
            .pop()
            .expect("the first band holds the header");
        plan.nodes.push(NodeOfPlan {
            raster_space_index: RasterSpaceIndex::VIEWPORT,
            composite_clips: vec![CompositeClip {
                raster_space_index: RasterSpaceIndex::VIEWPORT,
                rect: IntRect::new(0, 0, 800, 50),
                corner_radii: libgfx_rust::CornerRadii::default(),
            }],
            composited_effect: None,
            parent_index: None,
            reach_in_parent_node: Reach::Anywhere,
        });
        plan.bands.push(BandOfPlan {
            placement_node_index: PlacementNodeIndex(plan.nodes.len() - 1),
            record_spans: vec![header_span],
            reach_of_layer_below_all_cuts: IntRect::new(0, 0, 800, 50),
            cuts: Vec::new(),
        });
    }

    type Mutation = fn(&mut LayeredPlan);

    // The need a mutation breaks, the page whose plan it mutates, and the mutation.
    type Rejection = (&'static str, fn() -> Page, Mutation);

    fn rejections() -> Vec<Rejection> {
        fn space_count(plan: &LayeredPlan) -> usize {
            plan.tiles_hold_content_moving_within_space_by_space_index.len()
        }
        fn every_band_holds_every_record_the_bands_hold(plan: &mut LayeredPlan) {
            let spans_of_bands = plan.bands.iter().flat_map(|band| &band.record_spans);
            let every_record_the_bands_hold = ListSpan {
                begin: spans_of_bands
                    .clone()
                    .map(|span| span.begin)
                    .min()
                    .expect("a band holds records"),
                end: spans_of_bands.map(|span| span.end).max().expect("a band holds records"),
            };
            for band in &mut plan.bands {
                band.record_spans = vec![every_record_the_bands_hold];
            }
        }
        vec![
            ("node of a band out of range", page_with_two_canvases, |plan| {
                plan.bands[0].placement_node_index = PlacementNodeIndex(plan.nodes.len());
            }),
            ("space of a node out of range", page_with_two_canvases, |plan| {
                plan.nodes[0].raster_space_index = RasterSpaceIndex(space_count(plan));
            }),
            (
                "space of a composite clip out of range",
                page_with_two_canvases,
                |plan| {
                    let raster_space_index = RasterSpaceIndex(space_count(plan));
                    plan.nodes[0].composite_clips.push(CompositeClip {
                        raster_space_index,
                        rect: IntRect::new(0, 0, 10, 10),
                        corner_radii: libgfx_rust::CornerRadii::default(),
                    });
                },
            ),
            ("node in itself", page_with_two_canvases, |plan| {
                plan.nodes[1].parent_index = Some(PlacementNodeIndex(1));
            }),
            (
                "composited node in no node",
                page_with_nested_composited_effects,
                |plan| {
                    let node = composited_node_indices(plan)[0];
                    plan.nodes[node].parent_index = None;
                },
            ),
            (
                "composited effect out of range",
                page_with_nested_composited_effects,
                |plan| {
                    let node = composited_node_indices(plan)[0];
                    plan.nodes[node].composited_effect = Some(EffectNodeIndex(9999));
                },
            ),
            (
                "composited effects nested otherwise than in the tree",
                page_with_nested_composited_effects,
                |plan| {
                    let [first, second] = composited_node_indices(plan)[..] else {
                        unreachable!()
                    };
                    let first_effect = plan.nodes[first].composited_effect;
                    plan.nodes[first].composited_effect = plan.nodes[second].composited_effect;
                    plan.nodes[second].composited_effect = first_effect;
                },
            ),
            (
                "media layer at a run that draws a fill",
                page_with_two_canvases,
                |plan| {
                    let (_, _, record_span, _) = first_media_of(plan);
                    record_span.begin = ListPosition::default();
                },
            ),
            (
                "media layer that names another canvas",
                page_with_two_canvases,
                |plan| *first_media_of(plan).0 = LiveContent::Canvas(CanvasId(77)),
            ),
            ("media layer of an image", page_with_two_canvases, |plan| {
                *first_media_of(plan).0 = LiveContent::ImageFrame(ImageFrameResourceId(1));
            }),
            ("span that starts inside a run", page_with_two_canvases, |plan| {
                let band = band_with_spans(plan);
                plan.bands[band].record_spans[0].begin.offset += 1;
            }),
            ("span in a chunk the list does not hold", plain_page, |plan| {
                let band = band_with_spans(plan);
                plan.bands[band].record_spans[0].end.chunk_index = ChunkIndexInList(999);
            }),
            ("span that ends before it begins", plain_page, |plan| {
                let band = band_with_spans(plan);
                let span = &mut plan.bands[band].record_spans[0];
                std::mem::swap(&mut span.begin, &mut span.end);
            }),
            ("media layer that starts inside a run", page_with_two_canvases, |plan| {
                first_media_of(plan).2.begin.offset += 4
            }),
            ("spans out of paint order", page_with_two_canvases, |plan| {
                let band = band_with_cuts(plan);
                plan.bands[band].record_spans.reverse();
            }),
            ("cuts out of paint order", page_with_two_canvases, |plan| {
                let band = band_with_cuts(plan);
                plan.bands[band].cuts.reverse();
            }),
            ("two layers of one identity", page_drawing_one_canvas_twice, |plan| {
                let band = band_with_cuts(plan);
                let BarrierOfPlan::Media { ordinal, .. } = &mut plan.bands[band].cuts[1].barrier else {
                    unreachable!()
                };
                *ordinal = 0;
            }),
            ("reach of a layer past the limit", page_with_two_canvases, |plan| {
                plan.bands[0].reach_of_layer_below_all_cuts = IntRect::new(i32::MAX - 10, 0, 100, 100);
            }),
            ("rect of negative size", page_with_two_canvases, |plan| {
                plan.bands[0].reach_of_layer_below_all_cuts = IntRect::new(0, 0, -10, 100);
            }),
            (
                "reach of a layer above a cut past the limit",
                page_with_two_canvases,
                |plan| {
                    let band = band_with_cuts(plan);
                    plan.bands[band].cuts[0].reach_of_layer_above_cut = IntRect::new(0, i32::MIN, 1, 1);
                },
            ),
            ("reach of a node past the limit", page_with_two_canvases, |plan| {
                plan.nodes[0].reach_in_parent_node = Reach::Within(IntRect::new(i32::MIN, i32::MIN, 10, 10));
            }),
            ("reach of a canvas past the limit", page_with_two_canvases, |plan| {
                *first_media_of(plan).3 = IntRect::new(0, 0, i32::MAX, 1);
            }),
            ("negative radius of a composite clip", page_with_two_canvases, |plan| {
                plan.nodes[1].composite_clips.push(CompositeClip {
                    raster_space_index: RasterSpaceIndex(0),
                    rect: IntRect::new(0, 0, 10, 10),
                    corner_radii: libgfx_rust::CornerRadii::uniform(-5),
                });
            }),
            ("swept reach past the limit", page_with_spinner, |plan| {
                plan.swept_reach_in_parent_space_by_space_index[1] = Some(IntRect::new(1 << 30, 0, 1, 1));
            }),
            ("a value per space missing", page_with_two_canvases, |plan| {
                plan.tiles_hold_content_moving_within_space_by_space_index.pop();
            }),
            ("a swept reach per space too many", page_with_two_canvases, |plan| {
                plan.swept_reach_in_parent_space_by_space_index.push(None)
            }),
            (
                "a swept reach of a space the compositor does not animate",
                page_with_two_canvases,
                |plan| plan.swept_reach_in_parent_space_by_space_index[1] = Some(IntRect::new(0, 0, 1, 1)),
            ),
            ("record held by two bands", plain_page, |plan| {
                let span_of_document = plan.bands[1].record_spans[0];
                plan.bands[0].record_spans.insert(0, span_of_document);
            }),
            (
                "records held by every band",
                plain_page,
                every_band_holds_every_record_the_bands_hold,
            ),
            (
                "records held by every band",
                page_with_canvas_in_nested_scroller,
                every_band_holds_every_record_the_bands_hold,
            ),
            (
                "record held by a band and by a media layer",
                page_with_two_canvases,
                |plan| {
                    let (band_index, _) = first_media_cut(plan);
                    let record_span = *first_media_of(plan).2;
                    let band = &mut plan.bands[band_index];
                    let position = band.record_spans.partition_point(|span| span.end <= record_span.begin);
                    band.record_spans.insert(position, record_span);
                },
            ),
            ("span that holds no record", page_with_two_canvases, |plan| {
                let band = band_with_spans(plan);
                let end_of_first_span = plan.bands[band].record_spans[0].end;
                plan.bands[band].record_spans.insert(
                    1,
                    ListSpan {
                        begin: end_of_first_span,
                        end: end_of_first_span,
                    },
                );
            }),
            ("band other than the first that holds nothing", plain_page, |plan| {
                plan.bands[1].record_spans.clear()
            }),
            (
                "more media layers than a plan makes",
                page_with_one_scroll_bar_past_the_most_that_get_layers,
                give_last_record_of_band_of_first_media_a_layer,
            ),
            (
                "two nodes composited inside one effect",
                page_with_two_fading_siblings,
                |plan| {
                    let [first, second] = composited_node_indices(plan)[..] else {
                        unreachable!()
                    };
                    plan.nodes[second].composited_effect = plan.nodes[first].composited_effect;
                },
            ),
            ("node in a node of its own space", plain_page, |plan| {
                plan.nodes[1].raster_space_index = RasterSpaceIndex::VIEWPORT;
            }),
            (
                "composited node in a node of another space",
                page_with_nested_composited_effects,
                |plan| {
                    let node = composited_node_indices(plan)[0];
                    plan.nodes[node].raster_space_index = RasterSpaceIndex::VIEWPORT;
                },
            ),
            ("no band", plain_page, |plan| plan.bands.clear()),
            (
                "bands in a band out of the order of their cuts",
                page_with_two_fading_siblings,
                |plan| {
                    let [first, second] = composited_node_indices(plan)[..] else {
                        unreachable!()
                    };
                    let (first_band, second_band) = (band_of_node(plan, first), band_of_node(plan, second));
                    plan.bands.swap(first_band, second_band);
                },
            ),
            ("cut that enters no band", plain_page, |plan| {
                plan.bands
                    .last_mut()
                    .expect("the plan has a band")
                    .cuts
                    .push(CutOfPlan {
                        barrier: BarrierOfPlan::NestedBand,
                        reach_of_layer_above_cut: IntRect::default(),
                    });
            }),
            (
                "band that no cut enters",
                plain_page,
                move_header_into_band_no_cut_enters,
            ),
            (
                "band entered at a cut of a band of a node it is not in",
                plain_page,
                |plan| plan.bands[1].placement_node_index = PlacementNodeIndex(0),
            ),
            ("first band of a node in another node", plain_page, |plan| {
                plan.bands[0].placement_node_index = PlacementNodeIndex(1);
            }),
            (
                "first drawing of a canvas numbered as if one came before it",
                page_with_two_canvases,
                |plan| *first_media_of(plan).1 = 1,
            ),
            (
                "second drawing of a canvas numbered past the bits a sublayer keeps for it",
                page_drawing_one_canvas_twice,
                |plan| {
                    let band = band_with_cuts(plan);
                    let BarrierOfPlan::Media { ordinal, .. } = &mut plan.bands[band].cuts[1].barrier else {
                        unreachable!()
                    };
                    *ordinal = 1 << 16;
                },
            ),
        ]
    }

    #[test]
    fn a_plan_that_a_consumer_cannot_read_safely_is_rejected() {
        for (need, page, mutate) in rejections() {
            let page = page();
            let mut plan = page.planned_plan();
            assert!(
                page.validated_plan(plan.clone()).is_ok(),
                "{need}: the planned stack passes"
            );
            mutate(&mut plan);
            assert!(
                page.validated_plan(plan).is_err(),
                "{need}: the mutated plan is rejected"
            );
        }
    }

    // The layer the band's record at `record_position` goes into within the tile at `cell`: the one above the last barrier
    // painted before it that cuts the tile.
    fn layer_of_record_in_cell(band: &PlacementNodeBand, record_position: ListPosition, cell: IntRect) -> LayerIndex {
        band.cuts
            .iter()
            .take_while(|cut| cut.barrier_first_record_position <= record_position)
            .filter(|cut| cut.cuts_cell(cell))
            .last()
            .map_or(band.layer_below_all_cuts, |cut| cut.layer_above_cut)
    }

    // The cuts of a cell are worked out once for the cell, and a record finds its layer among them.
    #[test]
    fn the_cuts_of_a_cell_place_each_record_in_the_layer_the_band_places_it_in() {
        let pages = every_page().into_iter().chain([
            ("canvas drawn twice", page_drawing_one_canvas_twice()),
            ("nine canvases", page_with_nine_canvases()),
        ]);
        for (name, page) in pages {
            let stack = page.planned().stack;
            let record_positions: Vec<ListPosition> = (0..page.list.run_count())
                .map(|run_index| page.list.span_of_run(run_index).begin)
                .collect();
            for band in &stack.bands {
                for (column, row) in (-1..3).flat_map(|column| (-1..6).map(move |row| (column, row))) {
                    let cell = crate::display_list::tiles::TileCoord::new(column, row).cell_rect();
                    let cuts_of_cell = CutsOfCell::of(band, cell);
                    for &position in &record_positions {
                        let (layer, _) = cuts_of_cell.layer_of_record(position);
                        assert_eq!(layer, layer_of_record_in_cell(band, position, cell), "{name}");
                    }
                }
            }
        }
    }

    // Reads the stack as the compositor reads it for a frame over the whole viewport, and as the dump reads it.
    // A window server that shows tiles shows a caret from a surface of its own, as it shows a tile, and the base layer
    // with it. Any other leaves the caret to the frame.
    #[test]
    fn only_a_window_server_that_shows_tiles_shows_a_caret() {
        let stack = page_with_carets().planned().stack;
        let composite_offsets = stack.composite_offsets(&[FloatPoint::default(); 4]);
        let window_server = |shows_tiles_of_painted_layers| WhatWindowServerShowsOfFrame {
            compositing: WindowServerCompositing {
                shows_tiles_of_painted_layers,
                filters_what_is_below_layers: shows_tiles_of_painted_layers,
            },
            can_show_media_of_layer: &|_| true,
            can_composite_effect_of_node: &|_| true,
        };
        let caret_layers: Vec<LayerIndex> = (0..stack.layers.len())
            .map(LayerIndex)
            .filter(|layer| matches!(stack.layers[layer.0].content, LayerContent::OwnSurface { .. }))
            .collect();
        assert_eq!(caret_layers.len(), 2);
        let shown_by_window_server_that_shows_tiles =
            stack.layers_the_window_server_shows(&composite_offsets, &window_server(true));
        assert!(shown_by_window_server_that_shows_tiles[stack.base_layer.0]);
        assert!(
            caret_layers
                .iter()
                .all(|layer| shown_by_window_server_that_shows_tiles[layer.0])
        );
        for layer in caret_layers {
            assert_eq!(
                stack.why_frame_holds_layer(layer, &window_server(false)),
                WhyFrameHoldsLayer::WindowServerCannotShowItsContent
            );
        }
    }

    fn read_as_consumers_do(page: &Page, frame_tree: &VisualContextTree, planned: &PlannedLayers, stack: &LayerStack) {
        let list = &page.list;
        let scroll_offsets = [FloatPoint { x: 0.0, y: 300.0 }; 4];
        let composite_offsets = stack.composite_offsets(&scroll_offsets);
        let spaces_to_frame = stack.spaces_to_frame(&composite_offsets, |_| {
            libgfx_rust::affine_to_matrix(libgfx_rust::AffineTransform::new(0.5, 0.5, -0.5, 0.5, 10.0, 20.0))
        });
        for layer_index in (0..stack.layers.len()).map(LayerIndex) {
            stack.composited_bands_around_layer_innermost_first(layer_index).count();
            stack.composite_clip_of_layer_in_viewport(layer_index, &composite_offsets);
            stack.rounded_composite_clips_of_node_in_viewport(
                stack.layers[layer_index.0].placement_node_index,
                &composite_offsets,
            );
            stack.layer_is_in_animated_space(layer_index);
            stack.name_of_layer(layer_index, &NodeNamesFromIndices);
            let space_to_frame = spaces_to_frame[stack.space_of_layer(layer_index).0];
            space_to_frame.rect_in_frame(stack.layers[layer_index.0].reach);
            space_to_frame.rect_in_space(VIEWPORT);
        }
        for band_index in (0..stack.bands.len()).map(BandIndex) {
            stack.band_has_layer_at_or_above_base_layer(band_index);
        }
        let mut presentation = WindowServerPresentationOfMedia::default();
        for shows_tiles_of_painted_layers in [false, true] {
            let window_server_compositing = WindowServerCompositing {
                shows_tiles_of_painted_layers,
                filters_what_is_below_layers: shows_tiles_of_painted_layers,
            };
            stack.layers_the_window_server_shows(
                &composite_offsets,
                &WhatWindowServerShowsOfFrame {
                    compositing: window_server_compositing,
                    can_show_media_of_layer: &|_| true,
                    can_composite_effect_of_node: &|_| true,
                },
            );
            presentation = stack.window_server_presentation_of_media(
                list,
                frame_tree,
                &scroll_offsets,
                VIEWPORT,
                window_server_compositing,
            );
        }
        let planned_with_stack = PlannedLayers {
            stack: stack.clone(),
            placement: planned.placement.clone(),
        };
        planned_with_stack.layer_lines(&presentation, &NodeNamesFromIndices, |layer| {
            Some(format!("{}", layer.0))
        });
        planned_with_stack.layer_lines(
            &WindowServerPresentationOfMedia::default(),
            &NodeNamesFromIndices,
            |_| None,
        );
        for run_index in 0..list.run_count() {
            planned_with_stack.layers_of_record(list.span_of_run(run_index).begin);
        }
        planned_with_stack.note_lines_for_layers_overlay();
        for layer_index in 0..stack.layers.len() {
            stack.space_holds_content_that_moves_within_it(stack.space_of_layer(LayerIndex(layer_index)));
        }

        let tile_keying_of_list = TileKeyingOfList::new(list, frame_tree, stack, NO_FRAME_STATE);
        let raster_spaces = RasterSpaces::of_spaces(frame_tree, stack.spaces.clone());
        let what_every_space_of_tree_shares = WhatEverySpaceOfTreeShares::of(frame_tree);
        let geometries: Vec<RasterSpaceGeometry> = (0..stack.spaces.len())
            .map(|space_index| {
                RasterSpaceGeometry::new(
                    frame_tree,
                    &raster_spaces,
                    RasterSpaceIndex(space_index),
                    &scroll_offsets,
                    FloatMatrix4x4::identity(),
                    &what_every_space_of_tree_shares,
                )
            })
            .collect();
        let tile_keying_by_space: Vec<TileKeyingOfRasterSpace<'_>> = geometries
            .iter()
            .map(|geometry| TileKeyingOfRasterSpace::new(&tile_keying_of_list, geometry))
            .collect();
        let raster_settings_hash_by_layer = raster_settings_hash_by_layer(stack);
        for (band_position, band) in stack.bands.iter().enumerate() {
            let band_index = BandIndex(band_position);
            if !stack.band_has_layer_at_or_above_base_layer(band_index) {
                continue;
            }
            let space_index = stack.nodes[band.placement_node_index.0].raster_space_index;
            let tile_keying = &tile_keying_by_space[space_index.0];
            let shown_in_viewport = stack
                .composite_clip_of_layer_in_viewport(band.layer_below_all_cuts, &composite_offsets)
                .map_or(VIEWPORT, |clip| VIEWPORT.intersected(clip));
            let shown_rect = spaces_to_frame[space_index.0].rect_in_space(shown_in_viewport);
            let mut band_tile_keys = tile_keying.bind_band(stack, band_index);
            for _ in 0..2 {
                tile_keying.key_shown_tiles(
                    &mut band_tile_keys,
                    stack,
                    band_index,
                    &raster_settings_hash_by_layer,
                    shown_rect,
                    shown_rect,
                    Some(VIEWPORT),
                );
            }
            for tile in band_tile_keys.tiles_within(shown_rect) {
                assert!(tile.layer_index.0 < stack.layers.len());
                tile_keying.coverage_of_tile(&band_tile_keys, tile, tile.coord.cell_rect(), Color::TRANSPARENT);
                band_tile_keys.record_spans_of_tiles([tile]);
            }
        }
        for composited in &stack.composited_effects {
            let space_index = stack.nodes[composited.placement_node_index.0].raster_space_index;
            tile_keying_by_space[space_index.0].content_hash_of_composited_effect(composited.effect);
            crate::display_list::effect_geometry::backdrop_filter_read_reach(
                frame_tree,
                &geometries[space_index.0],
                composited.effect,
            );
        }
        for (layer_index, layer) in stack.layers.iter().enumerate() {
            if let LayerContent::Media { record_span, .. } = layer.content {
                tile_keying_by_space[stack.space_of_layer(LayerIndex(layer_index)).0]
                    .content_hash_of_records_within(record_span);
            }
        }
    }

    // A small generator of numbers that are far from random but alike from one run to the next.
    struct Xorshift(u64);

    impl Xorshift {
        fn next(&mut self) -> u64 {
            self.0 ^= self.0 << 13;
            self.0 ^= self.0 >> 7;
            self.0 ^= self.0 << 17;
            self.0
        }

        fn below(&mut self, bound: usize) -> usize {
            (self.next() % bound as u64) as usize
        }
    }

    const BOUNDARY_VALUES: [u32; 12] = [
        0,
        1,
        2,
        3,
        u32::MAX,
        i32::MIN as u32,
        i32::MAX as u32,
        0x7fc0_0000,
        0x7f80_0000,
        0xff80_0000,
        0x7149_f2ca,
        0x0080_0000,
    ];

    fn mutated(bytes: &[u8], generator: &mut Xorshift) -> Vec<u8> {
        let mut bytes = bytes.to_vec();
        for _ in 0..=generator.below(3) {
            if bytes.is_empty() {
                break;
            }
            match generator.below(5) {
                0 => {
                    let position = generator.below(bytes.len());
                    bytes[position] ^= 1 << generator.below(8);
                }
                1 if bytes.len() >= 4 => {
                    let position = generator.below(bytes.len() - 3);
                    let value = BOUNDARY_VALUES[generator.below(BOUNDARY_VALUES.len())];
                    bytes[position..position + 4].copy_from_slice(&value.to_ne_bytes());
                }
                2 => {
                    let (first, second) = (generator.below(bytes.len()), generator.below(bytes.len()));
                    bytes.swap(first, second);
                }
                3 => {
                    let position = generator.below(bytes.len());
                    bytes[position] = generator.next() as u8;
                }
                _ => bytes.truncate(generator.below(bytes.len())),
            }
        }
        bytes
    }

    // A candidate turned away takes no space, so candidates after it still get one, however many are turned away.
    #[test]
    fn anchors_are_chosen_past_candidates_that_are_turned_away() {
        let (mut tree, _, contexts) = tree_with_document_scroller(&[]);
        let document = contexts[0].spatial;
        let scrollport = IntRect::new(0, 0, 100, 100);
        let mut records = tree.compositor_scroll_table().nodes.to_vec();
        let mut scrollers_without_wheel = Vec::new();
        let mut innermost_scroller = document;
        for _ in 0..8 {
            let node = scroll_node(&mut tree, innermost_scroller);
            let mut record = scroll_record(node, innermost_scroller, scrollport, 300.0, false);
            record.can_be_wheel_scrolled_vertically = false;
            records.push(record);
            scrollers_without_wheel.push(node);
            innermost_scroller = node;
        }
        let scroller_inside_too_many_scrollers = scroll_node(&mut tree, innermost_scroller);
        records.push(scroll_record(
            scroller_inside_too_many_scrollers,
            innermost_scroller,
            scrollport,
            300.0,
            false,
        ));
        let scroller_in_document = scroll_node(&mut tree, document);
        records.push(scroll_record(scroller_in_document, document, scrollport, 300.0, false));
        assert!(tree.set_compositor_scroll_table(CompositorScrollTable {
            nodes: records.into(),
            ..CompositorScrollTable::default()
        }));
        let mut animations = Vec::new();
        for _ in 0..2000 {
            let node = tree.append_spatial(
                SpatialData::Transform(transform(FloatRect::new(0.0, 0.0, 10.0, 10.0))),
                document,
            );
            tree.append_effect(
                EffectNodeData::Effects(EffectsData {
                    isolation: false,
                    opacity: 1.0,
                    blend_mode: CompositingAndBlendingOperator::Multiply,
                    filter: None,
                    backdrop_filter: None,
                }),
                EffectNodeIndex::NONE,
                node,
                ClipNodeIndex::NONE,
            );
            animations.push(spinning(node));
        }
        let spinners: Vec<SpatialNodeIndex> = (0..3)
            .map(|_| {
                let node = tree.append_spatial(
                    SpatialData::Transform(transform(FloatRect::new(0.0, 0.0, 10.0, 10.0))),
                    document,
                );
                animations.push(spinning(node));
                node
            })
            .collect();
        tree.set_visual_animations(animations);

        let anchors = anchors_of_layer_plans(&tree);
        let [first, second, third, fourth, fifth, ..] = scrollers_without_wheel[..] else {
            unreachable!();
        };
        let mut expected_anchors = vec![document, first, scroller_in_document, second, third, fourth, fifth];
        expected_anchors.extend(&spinners);
        assert_eq!(anchors, expected_anchors);
        let parent_indices: Vec<usize> = RasterSpaces::spaces_at_anchors(&tree, &anchors, VIEWPORT)
            .iter()
            .skip(1)
            .map(|space| space.parent_index.0)
            .collect();
        assert_eq!(parent_indices, [0, 1, 1, 2, 4, 5, 6, 1, 1, 1]);
    }

    #[test]
    fn a_mutated_plan_that_passes_its_checks_is_read_without_a_panic() {
        let mut generator = Xorshift(0x9e37_79b9_7f4a_7c15);
        let mut validated_count = 0;
        for (name, page) in every_page() {
            let planned = page.planned();
            let frame_tree = page.tree.with_visual_animation_samples(250_000_000);
            read_as_consumers_do(&page, &frame_tree, &planned, &planned.stack);
            let bytes = encoded(&plan_sent_of(&planned));
            for trial in 0..3000 {
                let mutated_bytes = mutated(&bytes, &mut generator);
                let Ok(plan) = decoded_and_validated_plan(&page, &mutated_bytes) else {
                    continue;
                };
                validated_count += 1;
                let reading = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    read_as_consumers_do(&page, &frame_tree, &planned, &plan.stack);
                }));
                assert!(reading.is_ok(), "trial {trial} of the {name} panics: {:?}", plan.stack);
            }
        }
        assert!(validated_count > 0, "some mutated plans pass their checks");
    }
}
