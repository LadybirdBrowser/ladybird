/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::display_list::builder::{HEADER_SIZE, for_each_command, read_command, read_header};
use crate::display_list::commands::{
    ClipMode, ClipNodeIndex, DeclareMaskContent, DisplayListCommandRun, DisplayListCommandType, EffectNodeIndex,
};
use crate::display_list::effect_clip_plan::EffectClipPlan;
use crate::display_list::effect_geometry::{Extent, compute_effect_geometry};
use crate::display_list::summary::{DisplayListSummary, IndexedRecord, LiveContent};
use crate::display_list::window_server_placement::{
    WindowServerPlacement, WindowServerRejection, window_server_placement,
};
use crate::host::RecordSpan;
use crate::visual_context::queries::TreeCullingScratch;
use crate::visual_context::{ClipNodeData, EffectNodeData, SpatialData, VisualContextTree, WorldTransforms};
use libgfx_rust::{FloatMatrix4x4, FloatPoint, FloatRect, IntRect, enclosing_int_rect};

pub const MAXIMUM_MEDIA_LAYER_COUNT: usize = 8;
const BOUNDS_MARGIN_IN_DEVICE_PIXELS: i32 = 3;
const MAXIMUM_UPPER_PAINTED_LAYER_AREA_IN_VIEWPORTS: i64 = 2;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LayerKind {
    Base,
    Painted,
    Media { content: LiveContent },
}

impl std::fmt::Display for LayerKind {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            LayerKind::Base => formatter.write_str("base"),
            LayerKind::Painted => formatter.write_str("painted"),
            LayerKind::Media { content } => formatter.write_str(media_name(*content)),
        }
    }
}

fn media_name(content: LiveContent) -> &'static str {
    match content {
        LiveContent::Canvas(_) => "canvas",
        LiveContent::VideoFrame(_) => "video",
        LiveContent::ImageFrame(_) => "image",
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MediaLayerRejection {
    DrawnInsideAnotherCommand,
    InsideGroupEffect,
    UnderTransformThatIsNot2DAffine,
    DrawsNothingVisible,
    BetweenRunsOfOneGroupEffect,
    UnderBackdropFilterReadingRootGroup,
}

impl std::fmt::Display for MediaLayerRejection {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(match self {
            MediaLayerRejection::DrawnInsideAnotherCommand => "drawn inside another command",
            MediaLayerRejection::InsideGroupEffect => "inside a group effect",
            MediaLayerRejection::UnderTransformThatIsNot2DAffine => "under a transform that is not 2D affine",
            MediaLayerRejection::DrawsNothingVisible => "draws nothing visible",
            MediaLayerRejection::BetweenRunsOfOneGroupEffect => "between the runs of one group effect",
            MediaLayerRejection::UnderBackdropFilterReadingRootGroup => {
                "under a backdrop filter that reads the root group"
            }
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UnlayeredReason {
    NoActiveMedia,
    EmptyViewport,
    EmptyDisplayList,
    SortingContext3D,
    RootIsolationEffectIsNotPlainGroup,
    BlendModeReadsRootGroup,
    CommandBlendsWithRootGroup,
    MaskDeclaredOutsideItsMask,
    ActiveMediaNotDrawn,
    NoMediaCanBeLayer {
        first_rejected_media: LiveContent,
        rejection: MediaLayerRejection,
    },
    UpperPaintedLayersTooLarge,
}

impl std::fmt::Display for UnlayeredReason {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let text = match self {
            UnlayeredReason::NoMediaCanBeLayer {
                first_rejected_media,
                rejection,
            } => return write!(formatter, "{} {rejection}", media_name(*first_rejected_media)),
            UnlayeredReason::UpperPaintedLayersTooLarge => {
                return write!(
                    formatter,
                    "painted layers larger than {MAXIMUM_UPPER_PAINTED_LAYER_AREA_IN_VIEWPORTS} viewports"
                );
            }
            UnlayeredReason::NoActiveMedia => "no canvas or video keeps updating",
            UnlayeredReason::EmptyViewport => "empty viewport",
            UnlayeredReason::EmptyDisplayList => "empty display list",
            UnlayeredReason::SortingContext3D => "3D sorting context",
            UnlayeredReason::RootIsolationEffectIsNotPlainGroup => "root isolation effect is not a plain group",
            UnlayeredReason::BlendModeReadsRootGroup => "blend mode reads the root group",
            UnlayeredReason::CommandBlendsWithRootGroup => "command blends with the root group",
            UnlayeredReason::MaskDeclaredOutsideItsMask => "mask content declared outside its mask",
            UnlayeredReason::ActiveMediaNotDrawn => "no updating canvas or video is drawn",
        };
        formatter.write_str(text)
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct Layer {
    pub kind: LayerKind,
    pub rect: IntRect,
    pub record_spans: Vec<RecordSpan>,
    /// For a media layer, where the window server can show its content by itself, or why it cannot.
    pub window_server_placement: Option<Result<WindowServerPlacement, WindowServerRejection>>,
}

// The records from `begin_offset` up to the next range's start are drawn into the layer at `layer`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct LayerRecordRange {
    begin_offset: u32,
    layer: u8,
}

#[derive(Clone, Debug, PartialEq)]
pub struct LayerPlan {
    pub layers: Vec<Layer>,
    record_ranges_in_tape_order: Vec<LayerRecordRange>,
}

impl LayerPlan {
    pub fn media_record_offsets(&self) -> impl Iterator<Item = u32> + '_ {
        self.layers.iter().filter_map(|layer| match layer.kind {
            LayerKind::Media { .. } => Some(layer.record_spans[0].begin_offset),
            _ => None,
        })
    }

    /// Whether the plan has the same layers as `other` in the same order, so that a raster kept for a layer of one
    /// plan holds the content of the same layer of the other plan wherever no damage says otherwise.
    pub fn has_same_layers_as(&self, other: &LayerPlan) -> bool {
        self.layers.len() == other.layers.len()
            && self
                .layers
                .iter()
                .zip(&other.layers)
                .all(|(layer, other_layer)| layer.kind == other_layer.kind)
    }

    /// One line per layer, bottom to top: `[L<index>] <kind> rect=[<x>,<y> <width>x<height>]` after `indent`.
    pub fn write_layer_lines(&self, output: &mut impl std::fmt::Write, indent: &str) -> std::fmt::Result {
        for (index, layer) in self.layers.iter().enumerate() {
            let rect = layer.rect;
            write!(
                output,
                "{indent}[L{index}] {} rect=[{},{} {}x{}]",
                layer.kind, rect.x, rect.y, rect.width, rect.height
            )?;
            match &layer.window_server_placement {
                Some(Ok(placement)) => writeln!(output, " shown by the window server: {placement}")?,
                Some(Err(rejection)) => writeln!(output, " replayed: {rejection}")?,
                None => writeln!(output)?,
            }
        }
        Ok(())
    }

    pub fn layer_of_record(&self, record_offset: u32) -> usize {
        let range_index = self
            .record_ranges_in_tape_order
            .partition_point(|range| range.begin_offset <= record_offset)
            .saturating_sub(1);
        self.record_ranges_in_tape_order[range_index].layer as usize
    }

    /// Whether the records of `run` go into the same layers as those of `other_run`, an identical run of the list
    /// that `other` plans, at the same offsets in the run.
    pub fn places_run_like(
        &self,
        run: &DisplayListCommandRun,
        other: &LayerPlan,
        other_run: &DisplayListCommandRun,
    ) -> bool {
        self.layers_of_records_in(run).eq(other.layers_of_records_in(other_run))
    }

    // Where in the run the records of each layer start, as offsets in the run.
    fn layers_of_records_in(&self, run: &DisplayListCommandRun) -> impl Iterator<Item = (u32, u8)> + '_ {
        let first_range_index = self
            .record_ranges_in_tape_order
            .partition_point(|range| range.begin_offset <= run.offset)
            .saturating_sub(1);
        let (run_offset, run_end) = (run.offset, run.offset + run.size);
        std::iter::once((0, self.record_ranges_in_tape_order[first_range_index].layer)).chain(
            self.record_ranges_in_tape_order[first_range_index + 1..]
                .iter()
                .take_while(move |range| range.begin_offset < run_end)
                .map(move |range| (range.begin_offset - run_offset, range.layer)),
        )
    }
}

pub struct LayerPlanInputs<'a> {
    pub tape: &'a [u8],
    pub command_runs: &'a [DisplayListCommandRun],
    pub summary: &'a DisplayListSummary,
    pub effect_clips: &'a EffectClipPlan,
    pub tree: &'a VisualContextTree,
    pub scroll_offsets: &'a [FloatPoint],
    pub viewport_rect: IntRect,
    pub active_media: &'a [LiveContent],
}

fn tree_has_sorting_contexts(tree: &VisualContextTree) -> bool {
    tree.spatial_nodes.iter().any(|node| {
        matches!(&node.data, SpatialData::Transform(transform) if transform.sorting_context_root_index.is_some())
    })
}

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
        }
        _ => false,
    }
}

fn effect_chain_contains(tree: &VisualContextTree, mut effect: EffectNodeIndex, wanted: EffectNodeIndex) -> bool {
    while !effect.is_none() {
        if effect == wanted {
            return true;
        }
        effect = tree.effect_nodes[effect.0 as usize].parent;
    }
    false
}

fn what_reads_root_group(inputs: &LayerPlanInputs<'_>) -> Option<UnlayeredReason> {
    if inputs.tree.has_blend_mode_reading_root_group() {
        return Some(UnlayeredReason::BlendModeReadsRootGroup);
    }
    let command_blends_with_root_group = inputs.summary.destination_blending_records.iter().any(|record| {
        let effect = inputs.command_runs[record.run_index as usize].context.effect;
        inputs.tree.content_under_effect_composites_into_root_group(effect)
    });
    command_blends_with_root_group.then_some(UnlayeredReason::CommandBlendsWithRootGroup)
}

fn mask_declarations_sit_under_their_masks(inputs: &LayerPlanInputs<'_>) -> bool {
    inputs.summary.mask_declarations.iter().all(|record| {
        let offset = record.record_offset as usize;
        let header = read_header(&inputs.tape[offset..]);
        let payload = &inputs.tape[offset + HEADER_SIZE..offset + HEADER_SIZE + header.payload_size as usize];
        let declaration = read_command::<DeclareMaskContent>(payload);
        let run_effect = inputs.command_runs[record.run_index as usize].context.effect;
        effect_chain_contains(inputs.tree, run_effect, declaration.effect)
    })
}

// The structure decides the grouping effect rather than the effect values, so chunks stay put while an opacity animates.
fn outermost_grouping_effects(tree: &VisualContextTree) -> Vec<EffectNodeIndex> {
    let mut outermost = vec![EffectNodeIndex::NONE; tree.effect_nodes.len()];
    tree.visit_effect_nodes_parents_first(|index| {
        let node = &tree.effect_nodes[index];
        let from_parent = if node.parent.is_none() {
            EffectNodeIndex::NONE
        } else {
            outermost[node.parent.0 as usize]
        };
        let is_grouping = node.data.pushes_layer() && tree.root_isolation_effect != Some(EffectNodeIndex(index as u32));
        outermost[index] = if !from_parent.is_none() {
            from_parent
        } else if is_grouping {
            EffectNodeIndex(index as u32)
        } else {
            EffectNodeIndex::NONE
        };
    });
    outermost
}

// A chain holds at most one such effect, since the layer of its backdrop filter isolates every effect under it.
fn backdrop_filter_reading_root_group_on_chain(tree: &VisualContextTree) -> Vec<EffectNodeIndex> {
    let mut on_chain = vec![EffectNodeIndex::NONE; tree.effect_nodes.len()];
    tree.visit_effect_nodes_parents_first(|index| {
        let node = &tree.effect_nodes[index];
        let effect = EffectNodeIndex(index as u32);
        let own = matches!(&node.data, EffectNodeData::Effects(effects) if effects.backdrop_filter.is_some())
            && tree.effect_backdrop_is_root_group(effect);
        on_chain[index] = if own {
            effect
        } else if node.parent.is_none() {
            EffectNodeIndex::NONE
        } else {
            on_chain[node.parent.0 as usize]
        };
    });
    on_chain
}

fn clip_chain_bounds(tree: &VisualContextTree, world: &WorldTransforms) -> Vec<Option<FloatRect>> {
    let mut bounds: Vec<Option<FloatRect>> = vec![None; tree.clip_nodes.len()];
    tree.visit_clip_nodes_parents_first(|index| {
        let node = &tree.clip_nodes[index];
        let own = match &node.data {
            ClipNodeData::Rect(data) if data.mode == ClipMode::Intersect => Some(data.rect),
            ClipNodeData::Path(path) => Some(path.bounding_rect.to_float()),
            _ => None,
        }
        .map(|rect| world.map_rect_to_root(node.spatial, rect))
        .filter(|rect| rect.x.is_finite() && rect.y.is_finite() && rect.width.is_finite() && rect.height.is_finite());
        let from_parent = if node.parent.is_none() {
            None
        } else {
            bounds[node.parent.0 as usize]
        };
        bounds[index] = match (own, from_parent) {
            (Some(own), Some(from_parent)) => Some(own.intersected(from_parent)),
            (own, from_parent) => own.or(from_parent),
        };
    });
    bounds
}

struct BoundsContext<'a> {
    world: WorldTransforms,
    culling: TreeCullingScratch,
    clip_bounds: Vec<Option<FloatRect>>,
    viewport_rect: IntRect,
    tree: &'a VisualContextTree,
}

impl BoundsContext<'_> {
    fn device_rect(&self, rect: Option<FloatRect>, clip: ClipNodeIndex) -> IntRect {
        let clip_bounds = if clip.is_none() {
            None
        } else {
            self.clip_bounds[clip.0 as usize]
        };
        let rect = match (rect, clip_bounds) {
            (Some(rect), Some(clip_bounds)) => rect.intersected(clip_bounds),
            (Some(rect), None) => rect,
            (None, Some(clip_bounds)) => clip_bounds,
            (None, None) => return self.viewport_rect,
        };
        if !(rect.x.is_finite() && rect.y.is_finite() && rect.width.is_finite() && rect.height.is_finite()) {
            return self.viewport_rect;
        }
        if rect.is_empty() {
            return IntRect::default();
        }
        let viewport = self.viewport_rect.to_float();
        let rect = rect.intersected(viewport.inflated(
            2.0 * BOUNDS_MARGIN_IN_DEVICE_PIXELS as f32,
            2.0 * BOUNDS_MARGIN_IN_DEVICE_PIXELS as f32,
        ));
        if rect.is_empty() {
            return IntRect::default();
        }
        enclosing_int_rect(rect)
            .inflated_edges(
                BOUNDS_MARGIN_IN_DEVICE_PIXELS,
                BOUNDS_MARGIN_IN_DEVICE_PIXELS,
                BOUNDS_MARGIN_IN_DEVICE_PIXELS,
                BOUNDS_MARGIN_IN_DEVICE_PIXELS,
            )
            .intersected(self.viewport_rect)
    }

    fn ink_device_rect(&self, run: &DisplayListCommandRun, ink: IntRect) -> IntRect {
        if ink.is_empty()
            || self.world.backface_culled[run.context.spatial.0 as usize]
            || self.culling.context_culls_everything(run.context)
        {
            return IntRect::default();
        }
        let rect = self.world.map_rect_to_root(run.context.spatial, ink.to_float());
        self.device_rect(Some(rect), run.context.clip)
    }

    fn backdrop_filter_reads_device_rect(&self, effect: EffectNodeIndex, rect: IntRect) -> bool {
        let node = &self.tree.effect_nodes[effect.0 as usize];
        let EffectNodeData::Effects(effects) = &node.data else {
            return false;
        };
        let Some(backdrop_filter) = &effects.backdrop_filter else {
            return false;
        };
        if rect.is_empty() {
            return false;
        }
        let to_device = self.world.draw_matrix(node.spatial);
        if !to_device.is_2d_affine() {
            return true;
        }
        let Some(from_device) = to_device.extract_2d_affine().inverse() else {
            return true;
        };
        let rect_in_effect_space = from_device.map_rect(rect.to_float());
        crate::filter_bytes::changed_output_bounds(&backdrop_filter.filter, rect_in_effect_space)
            .is_none_or(|reached| !reached.intersected(backdrop_filter.region.to_float()).is_empty())
    }

    fn group_device_rect(&self, effect: EffectNodeIndex, output: Extent, effect_clips: &EffectClipPlan) -> IntRect {
        let node = &self.tree.effect_nodes[effect.0 as usize];
        let output_clip = effect_clips.output_clip(effect);
        match output {
            Extent::Nothing => IntRect::default(),
            Extent::Within(rect) => {
                self.device_rect(Some(self.world.map_rect_to_root(node.spatial, rect)), output_clip)
            }
            Extent::Anywhere => self.device_rect(None, output_clip),
        }
    }
}

// Bit `n` stands for the media layer of the `n`th canvas or video that the plan tries to give a layer.
type MediaLayerSet = u32;
const _: () = assert!(MAXIMUM_MEDIA_LAYER_COUNT <= MediaLayerSet::BITS as usize);

// The media layers that a backdrop filter reading the root group would read through, so that dropping them lets
// the backdrop filter go into the base layer.
struct MediaLayersUnderBackdropFilter(MediaLayerSet);

struct LayersBeingPlanned {
    layers: Vec<Layer>,
    // A media layer exists for itself, and a painted layer for the media layers that its content is drawn over.
    media_layers_each_layer_exists_for: Vec<MediaLayerSet>,
    record_ranges_in_tape_order: Vec<LayerRecordRange>,
}

impl LayersBeingPlanned {
    const BASE_LAYER_INDEX: usize = 0;

    fn new(viewport_rect: IntRect) -> Self {
        Self {
            layers: vec![Layer {
                kind: LayerKind::Base,
                rect: viewport_rect,
                record_spans: Vec::new(),
                window_server_placement: None,
            }],
            media_layers_each_layer_exists_for: vec![0],
            record_ranges_in_tape_order: Vec::new(),
        }
    }

    fn media_layers_that_layers_above_base_exist_for(&self, touches_layer: impl Fn(&Layer) -> bool) -> MediaLayerSet {
        (Self::BASE_LAYER_INDEX + 1..self.layers.len())
            .filter(|&index| touches_layer(&self.layers[index]))
            .fold(0, |media_layers, index| {
                media_layers | self.media_layers_each_layer_exists_for[index]
            })
    }

    // A backdrop filter reading the root group reads the base layer, so its content has to go there, clear of the
    // layers above it both where it draws and where it reads.
    fn check_backdrop_filters_can_go_into_base_layer(
        &self,
        bounds: IntRect,
        backdrop_filter_reads_layer: impl Fn(&Layer) -> bool,
    ) -> Result<(), MediaLayersUnderBackdropFilter> {
        let media_layers = self.media_layers_that_layers_above_base_exist_for(|layer| {
            !layer.rect.intersected(bounds).is_empty() || backdrop_filter_reads_layer(layer)
        });
        if media_layers == 0 {
            return Ok(());
        }
        Err(MediaLayersUnderBackdropFilter(media_layers))
    }

    fn topmost_layer_above_base_touching(&self, bounds: IntRect) -> Option<usize> {
        if bounds.is_empty() {
            return None;
        }
        (Self::BASE_LAYER_INDEX + 1..self.layers.len())
            .rev()
            .find(|&index| !self.layers[index].rect.intersected(bounds).is_empty())
    }

    fn add_media_layer(
        &mut self,
        media_index: usize,
        content: LiveContent,
        span: RecordSpan,
        bounds: IntRect,
        window_server_placement: Result<WindowServerPlacement, WindowServerRejection>,
    ) {
        self.layers.push(Layer {
            kind: LayerKind::Media { content },
            rect: bounds,
            record_spans: Vec::new(),
            window_server_placement: Some(window_server_placement),
        });
        self.media_layers_each_layer_exists_for.push(1 << media_index);
        self.append_records_to_layer(self.layers.len() - 1, span);
    }

    // Records drawn within `bounds` go into the lowest painted layer that is above every layer they touch, or into
    // the base layer when they touch none above it.
    fn place_painted_records(&mut self, span: RecordSpan, bounds: IntRect) -> usize {
        let layer_index = match self.topmost_layer_above_base_touching(bounds) {
            None => Self::BASE_LAYER_INDEX,
            Some(touched) => {
                let layer_index = (touched..self.layers.len())
                    .find(|&index| self.layers[index].kind == LayerKind::Painted)
                    .unwrap_or_else(|| {
                        self.layers.push(Layer {
                            kind: LayerKind::Painted,
                            rect: IntRect::default(),
                            record_spans: Vec::new(),
                            window_server_placement: None,
                        });
                        self.media_layers_each_layer_exists_for.push(0);
                        self.layers.len() - 1
                    });
                self.media_layers_each_layer_exists_for[layer_index] |= self
                    .media_layers_that_layers_above_base_exist_for(|layer| !layer.rect.intersected(bounds).is_empty());
                self.layers[layer_index].rect = self.layers[layer_index].rect.united(bounds);
                layer_index
            }
        };
        self.append_records_to_layer(layer_index, span);
        layer_index
    }

    fn append_records_to_layer(&mut self, layer_index: usize, span: RecordSpan) {
        let spans = &mut self.layers[layer_index].record_spans;
        match spans.last_mut() {
            Some(last) if last.end_offset == span.begin_offset => last.end_offset = span.end_offset,
            _ => spans.push(span),
        }
        if self
            .record_ranges_in_tape_order
            .last()
            .is_none_or(|range| range.layer as usize != layer_index)
        {
            self.record_ranges_in_tape_order.push(LayerRecordRange {
                begin_offset: span.begin_offset,
                layer: layer_index as u8,
            });
        }
    }
}

struct MediaCandidate {
    content: LiveContent,
    run_index: u32,
    record_offset: u32,
}

fn media_layer_rejection(
    inputs: &LayerPlanInputs<'_>,
    grouping_effects: &[EffectNodeIndex],
    bounds: &BoundsContext<'_>,
    run_is_a_chunk_of_its_own: impl Fn(u32) -> bool,
    record_offsets_of_media_under_backdrop_filters: &[u32],
    run_index: u32,
    record_offset: u32,
) -> Option<MediaLayerRejection> {
    let header = read_header(&inputs.tape[record_offset as usize..]);
    if !matches!(
        header.command_type,
        DisplayListCommandType::DrawCanvas | DisplayListCommandType::DrawVideoFrame
    ) {
        return Some(MediaLayerRejection::DrawnInsideAnotherCommand);
    }
    let run = &inputs.command_runs[run_index as usize];
    if !run.context.effect.is_none() && !grouping_effects[run.context.effect.0 as usize].is_none() {
        return Some(MediaLayerRejection::InsideGroupEffect);
    }
    if !bounds.world.draw_matrix(run.context.spatial).is_2d_affine() {
        return Some(MediaLayerRejection::UnderTransformThatIsNot2DAffine);
    }
    if bounds.ink_device_rect(run, header.bounding_rect).is_empty() {
        return Some(MediaLayerRejection::DrawsNothingVisible);
    }
    if !run_is_a_chunk_of_its_own(run_index) {
        return Some(MediaLayerRejection::BetweenRunsOfOneGroupEffect);
    }
    if record_offsets_of_media_under_backdrop_filters.contains(&record_offset) {
        return Some(MediaLayerRejection::UnderBackdropFilterReadingRootGroup);
    }
    None
}

fn media_candidates(
    inputs: &LayerPlanInputs<'_>,
    grouping_effects: &[EffectNodeIndex],
    bounds: &BoundsContext<'_>,
    run_is_a_chunk_of_its_own: impl Fn(u32) -> bool,
    record_offsets_of_media_under_backdrop_filters: &[u32],
) -> Result<Vec<MediaCandidate>, UnlayeredReason> {
    let mut candidates = Vec::new();
    let mut first_rejection = None;
    for live_content_record in &inputs.summary.live_content_records {
        let content = live_content_record.content;
        if matches!(content, LiveContent::ImageFrame(_)) || !inputs.active_media.contains(&content) {
            continue;
        }
        let IndexedRecord {
            run_index,
            record_offset,
        } = live_content_record.record;
        match media_layer_rejection(
            inputs,
            grouping_effects,
            bounds,
            &run_is_a_chunk_of_its_own,
            record_offsets_of_media_under_backdrop_filters,
            run_index,
            record_offset,
        ) {
            None => candidates.push(MediaCandidate {
                content,
                run_index,
                record_offset,
            }),
            Some(rejection) => {
                first_rejection.get_or_insert(UnlayeredReason::NoMediaCanBeLayer {
                    first_rejected_media: content,
                    rejection,
                });
            }
        }
    }
    if candidates.is_empty() {
        return Err(first_rejection.unwrap_or(UnlayeredReason::ActiveMediaNotDrawn));
    }
    Ok(candidates)
}

fn run_ranges_holding_whole_grouping_effects(
    command_runs: &[DisplayListCommandRun],
    grouping_of_run: &[EffectNodeIndex],
) -> Vec<(u32, u32)> {
    let mut last_run_of_group = std::collections::HashMap::new();
    for (run_index, group) in grouping_of_run.iter().enumerate() {
        if !group.is_none() {
            last_run_of_group.insert(group.0, run_index);
        }
    }
    let mut ranges = Vec::new();
    let mut run_index = 0;
    while run_index < command_runs.len() {
        let first = run_index;
        let mut last = run_index;
        let mut next = run_index;
        while next <= last {
            let group = grouping_of_run[next];
            if !group.is_none() {
                last = last.max(last_run_of_group[&group.0]);
            }
            next += 1;
        }
        ranges.push((first as u32, last as u32));
        run_index = last + 1;
    }
    ranges
}

// Runs that go into one layer together, as they hold all runs of the group effects among them.
struct Chunk {
    first_run: u32,
    last_run: u32,
    device_rect: IntRect,
    backdrop_filters_reading_root_group: Vec<EffectNodeIndex>,
}

fn place_records_in_layers(
    inputs: &LayerPlanInputs<'_>,
    bounds: &BoundsContext<'_>,
    chunks: &[Chunk],
    grouping_of_run: &[EffectNodeIndex],
    media: &[MediaCandidate],
) -> Result<LayersBeingPlanned, MediaLayersUnderBackdropFilter> {
    let command_runs = inputs.command_runs;
    let mut planned = LayersBeingPlanned::new(inputs.viewport_rect);
    let mut media_iter = media.iter().enumerate().peekable();
    for chunk in chunks {
        let (first, last) = (chunk.first_run, chunk.last_run);
        let first_run = &command_runs[first as usize];
        let last_run = &command_runs[last as usize];
        if !chunk.backdrop_filters_reading_root_group.is_empty() {
            planned.check_backdrop_filters_can_go_into_base_layer(chunk.device_rect, |layer| {
                chunk
                    .backdrop_filters_reading_root_group
                    .iter()
                    .any(|&effect| bounds.backdrop_filter_reads_device_rect(effect, layer.rect))
            })?;
        }

        let run_draws_media_layer = media_iter
            .peek()
            .is_some_and(|(_, candidate)| candidate.run_index == first);
        // The rect of a record under a perspective transform is an estimate, so such a run stays whole.
        let run_can_be_split_between_layers = first == last
            && grouping_of_run[first as usize].is_none()
            && bounds.world.draw_matrix(first_run.context.spatial).is_2d_affine();
        let places_each_record_of_run = run_draws_media_layer
            || (run_can_be_split_between_layers
                && planned.topmost_layer_above_base_touching(chunk.device_rect).is_some());
        if !places_each_record_of_run {
            let chunk_span = RecordSpan {
                begin_offset: first_run.offset,
                end_offset: last_run.offset + last_run.size,
            };
            planned.place_painted_records(chunk_span, chunk.device_rect);
            continue;
        }

        let run = first_run;
        let records = &inputs.tape[run.offset as usize..(run.offset + run.size) as usize];
        let mut layer_of_previous_painted_record = LayersBeingPlanned::BASE_LAYER_INDEX;
        for_each_command(records, |header, offset, payload| {
            let begin_offset = run.offset + offset as u32;
            let span = RecordSpan {
                begin_offset,
                end_offset: begin_offset + (HEADER_SIZE + payload.len()) as u32,
            };
            if let Some((media_index, candidate)) =
                media_iter.next_if(|(_, candidate)| candidate.record_offset == begin_offset)
            {
                let media_bounds = bounds.ink_device_rect(run, header.bounding_rect);
                let placement =
                    window_server_placement(inputs.tree, &bounds.world, inputs.viewport_rect, run, header, payload);
                planned.add_media_layer(media_index, candidate.content, span, media_bounds, placement);
                layer_of_previous_painted_record = LayersBeingPlanned::BASE_LAYER_INDEX;
            } else if !header.has_bounding_rect {
                planned.append_records_to_layer(layer_of_previous_painted_record, span);
            } else {
                let record_bounds = bounds.ink_device_rect(run, header.bounding_rect);
                layer_of_previous_painted_record = planned.place_painted_records(span, record_bounds);
            }
        });
    }
    Ok(planned)
}

pub fn build_layer_plan(inputs: &LayerPlanInputs<'_>) -> Result<LayerPlan, UnlayeredReason> {
    let tree = inputs.tree;
    let command_runs = inputs.command_runs;
    if inputs.active_media.is_empty() {
        return Err(UnlayeredReason::NoActiveMedia);
    }
    if inputs.viewport_rect.is_empty() {
        return Err(UnlayeredReason::EmptyViewport);
    }
    if command_runs.is_empty() {
        return Err(UnlayeredReason::EmptyDisplayList);
    }
    if tree_has_sorting_contexts(tree) {
        return Err(UnlayeredReason::SortingContext3D);
    }
    if !root_isolation_effect_is_plain_group(tree) {
        return Err(UnlayeredReason::RootIsolationEffectIsNotPlainGroup);
    }
    if let Some(reason) = what_reads_root_group(inputs) {
        return Err(reason);
    }
    if !mask_declarations_sit_under_their_masks(inputs) {
        return Err(UnlayeredReason::MaskDeclaredOutsideItsMask);
    }

    let mut culling = TreeCullingScratch::default();
    tree.fill_culling_scratch(&mut culling);
    let world = WorldTransforms::new(tree, inputs.scroll_offsets, Some(FloatMatrix4x4::identity()));
    let clip_bounds = clip_chain_bounds(tree, &world);
    let bounds = BoundsContext {
        world,
        culling,
        clip_bounds,
        viewport_rect: inputs.viewport_rect,
        tree,
    };

    let grouping_effects = outermost_grouping_effects(tree);
    let grouping_of_run: Vec<EffectNodeIndex> = command_runs
        .iter()
        .map(|run| {
            if run.context.effect.is_none() {
                EffectNodeIndex::NONE
            } else {
                grouping_effects[run.context.effect.0 as usize]
            }
        })
        .collect();
    let chunk_ranges = run_ranges_holding_whole_grouping_effects(command_runs, &grouping_of_run);
    let mut chunk_of_run = vec![0usize; command_runs.len()];
    for (chunk_index, &(first, last)) in chunk_ranges.iter().enumerate() {
        chunk_of_run[first as usize..=last as usize].fill(chunk_index);
    }

    let run_is_a_chunk_of_its_own = |run_index: u32| {
        let (first, last) = chunk_ranges[chunk_of_run[run_index as usize]];
        first == last
    };

    let mut effect_geometry = Vec::new();
    compute_effect_geometry(tree, command_runs, &bounds.world, &mut effect_geometry);
    let backdrop_filter_reading_root_group_on_chain = backdrop_filter_reading_root_group_on_chain(tree);
    let mut group_rects: std::collections::HashMap<u32, IntRect> = std::collections::HashMap::new();
    let mut run_device_rect = |run_index: usize| -> IntRect {
        let run = &command_runs[run_index];
        let group = grouping_of_run[run_index];
        if group.is_none() {
            return bounds.ink_device_rect(run, run.ink_bounds);
        }
        *group_rects.entry(group.0).or_insert_with(|| {
            bounds.group_device_rect(group, effect_geometry[group.0 as usize].output, inputs.effect_clips)
        })
    };
    let chunks: Vec<Chunk> = chunk_ranges
        .iter()
        .map(|&(first_run, last_run)| {
            let mut device_rect = IntRect::default();
            let mut backdrop_filters_reading_root_group = Vec::new();
            for (run_index, run) in command_runs
                .iter()
                .enumerate()
                .take(last_run as usize + 1)
                .skip(first_run as usize)
            {
                device_rect = device_rect.united(run_device_rect(run_index));
                let effect = run.context.effect;
                if effect.is_none() {
                    continue;
                }
                let backdrop_filter = backdrop_filter_reading_root_group_on_chain[effect.0 as usize];
                if !backdrop_filter.is_none() && !backdrop_filters_reading_root_group.contains(&backdrop_filter) {
                    backdrop_filters_reading_root_group.push(backdrop_filter);
                }
            }
            Chunk {
                first_run,
                last_run,
                device_rect,
                backdrop_filters_reading_root_group,
            }
        })
        .collect();

    // Each try leaves out at least one more canvas or video, so this ends.
    let mut record_offsets_of_media_under_backdrop_filters = Vec::new();
    let planned = loop {
        let mut media = media_candidates(
            inputs,
            &grouping_effects,
            &bounds,
            run_is_a_chunk_of_its_own,
            &record_offsets_of_media_under_backdrop_filters,
        )?;
        media.truncate(MAXIMUM_MEDIA_LAYER_COUNT);
        match place_records_in_layers(inputs, &bounds, &chunks, &grouping_of_run, &media) {
            Ok(planned) => break planned,
            Err(MediaLayersUnderBackdropFilter(media_layers)) => {
                record_offsets_of_media_under_backdrop_filters.extend(
                    media
                        .iter()
                        .enumerate()
                        .filter(|(media_index, _)| media_layers & (1 << media_index) != 0)
                        .map(|(_, candidate)| candidate.record_offset),
                );
            }
        }
    };
    let LayersBeingPlanned {
        layers,
        record_ranges_in_tape_order,
        ..
    } = planned;

    let viewport_area = i64::from(inputs.viewport_rect.width) * i64::from(inputs.viewport_rect.height);
    let upper_painted_area: i64 = layers
        .iter()
        .filter(|layer| layer.kind == LayerKind::Painted)
        .map(|layer| i64::from(layer.rect.width) * i64::from(layer.rect.height))
        .sum();
    if upper_painted_area > MAXIMUM_UPPER_PAINTED_LAYER_AREA_IN_VIEWPORTS * viewport_area {
        return Err(UnlayeredReason::UpperPaintedLayersTooLarge);
    }

    Ok(LayerPlan {
        layers,
        record_ranges_in_tape_order,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::display_list::builder::{DisplayListBuilder, PendingInlineClip};
    use crate::display_list::commands::{
        CanvasId, CompositorBlockingWheelEventRegion, ContextRef, DisplayListCommand, DrawCanvas, FillRect,
        SpatialNodeIndex, VISUAL_VIEWPORT_NODE_INDEX,
    };
    use crate::display_list::damage::{
        DisplayListDamage, DisplayListFrame, LayerPlans, LiveContentChanges, compute_display_list_damage,
    };
    use crate::display_list::summary::summarize;
    use crate::node_slot_id::NodeSlotId;
    use crate::visual_context::{EffectsData, ScrollData, TransformData, TransformDataRole};
    use libgfx_rust::{
        AffineTransform, Color, CompositingAndBlendingOperator, CornerRadii, ScalingMode, rotation_matrix, scale_matrix,
    };

    const VIEWPORT: IntRect = IntRect::new(0, 0, 800, 600);
    const CANVAS: LiveContent = LiveContent::Canvas(CanvasId(1));

    fn identity_tree() -> VisualContextTree {
        VisualContextTree::create(TransformData {
            matrix: FloatMatrix4x4::identity(),
            origin: FloatPoint::default(),
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
            bounds: FloatRect::default(),
        })
    }

    fn tree_with_root_isolation_effect() -> VisualContextTree {
        let mut tree = identity_tree();
        let root = tree.append_effect(
            effects(1.0, CompositingAndBlendingOperator::Normal),
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );
        tree.root_isolation_effect = Some(root);
        tree
    }

    fn effects(opacity: f32, blend_mode: CompositingAndBlendingOperator) -> EffectNodeData {
        EffectNodeData::Effects(EffectsData {
            isolation: false,
            opacity,
            blend_mode,
            filter: None,
            backdrop_filter: None,
        })
    }

    fn separate_clip(tree: &mut VisualContextTree, parent: ContextRef) -> ContextRef {
        let clip = tree.append_clip(
            ClipNodeData::rect_clip(VIEWPORT.to_float()),
            parent.clip,
            parent.spatial,
        );
        ContextRef { clip, ..parent }
    }

    fn fill(rect: IntRect) -> FillRect {
        FillRect {
            rect,
            color: Color::from_rgb(0, 128, 0),
            compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            background_color_animation_effect: EffectNodeIndex::NONE,
        }
    }

    fn canvas(rect: IntRect) -> DrawCanvas {
        DrawCanvas {
            dst_rect: rect,
            canvas_id: CanvasId(1),
            scaling_mode: ScalingMode::NearestNeighbor,
        }
    }

    struct TestList {
        builder: DisplayListBuilder,
    }

    impl TestList {
        fn new() -> Self {
            Self {
                builder: DisplayListBuilder::new(),
            }
        }

        fn with<C: DisplayListCommand>(mut self, command: C, context: ContextRef) -> Self {
            self.builder.append(&command, &[], context);
            self
        }

        fn with_inline_clips<C: DisplayListCommand>(
            mut self,
            command: C,
            context: ContextRef,
            inline_clips: &[PendingInlineClip],
        ) -> Self {
            self.builder
                .append_with_inline_clips(&command, &[], context, inline_clips);
            self
        }

        fn plan(&self, tree: &VisualContextTree, scroll_offsets: &[FloatPoint]) -> Result<LayerPlan, UnlayeredReason> {
            let (tape, runs) = (self.builder.bytes(), self.builder.command_runs());
            let summary = summarize(tape, runs);
            let effect_clips = EffectClipPlan::new(tree, runs).unwrap();
            build_layer_plan(&LayerPlanInputs {
                tape,
                command_runs: runs,
                summary: &summary,
                effect_clips: &effect_clips,
                tree,
                scroll_offsets,
                viewport_rect: VIEWPORT,
                active_media: &[CANVAS],
            })
        }
    }

    fn damage_with_layer_plans(
        tree: &VisualContextTree,
        (old_list, old_plan, old_scroll_offsets): (&TestList, &LayerPlan, &[FloatPoint]),
        (new_list, new_plan, new_scroll_offsets): (&TestList, &LayerPlan, &[FloatPoint]),
    ) -> DisplayListDamage {
        let old_summary = summarize(old_list.builder.bytes(), old_list.builder.command_runs());
        let new_summary = summarize(new_list.builder.bytes(), new_list.builder.command_runs());
        compute_display_list_damage(
            DisplayListFrame {
                command_bytes: old_list.builder.bytes(),
                command_runs: old_list.builder.command_runs(),
                summary: &old_summary,
                visual_context_tree: tree,
                scroll_offsets: old_scroll_offsets,
            },
            DisplayListFrame {
                command_bytes: new_list.builder.bytes(),
                command_runs: new_list.builder.command_runs(),
                summary: &new_summary,
                visual_context_tree: tree,
                scroll_offsets: new_scroll_offsets,
            },
            VIEWPORT,
            LiveContentChanges::default(),
            Some(LayerPlans {
                old: old_plan,
                new: new_plan,
            }),
        )
    }

    fn kinds(plan: &LayerPlan) -> Vec<&'static str> {
        plan.layers
            .iter()
            .map(|layer| match layer.kind {
                LayerKind::Base => "base",
                LayerKind::Painted => "painted",
                LayerKind::Media { .. } => "media",
            })
            .collect()
    }

    #[test]
    fn content_over_a_canvas_goes_above_it_and_other_content_stays_in_the_base_layer() {
        let mut tree = tree_with_root_isolation_effect();
        let root = ContextRef {
            effect: tree.root_isolation_effect.unwrap(),
            ..ContextRef::default()
        };
        let elsewhere = separate_clip(&mut tree, root);
        let list = TestList::new()
            .with(fill(IntRect::new(0, 0, 800, 600)), root)
            .with(canvas(IntRect::new(100, 100, 200, 100)), root)
            .with(fill(IntRect::new(150, 150, 20, 20)), root)
            .with(fill(IntRect::new(500, 400, 20, 20)), elsewhere);
        let plan = list.plan(&tree, &[]).unwrap();
        assert_eq!(kinds(&plan), ["base", "media", "painted"]);
        assert_eq!(plan.layers[1].rect, IntRect::new(97, 97, 206, 106));
        assert_eq!(plan.layers[2].rect, IntRect::new(147, 147, 26, 26));
        let run_end = |run: usize| list.builder.command_runs()[run].offset + list.builder.command_runs()[run].size;
        let canvas_span = plan.layers[1].record_spans[0];
        assert_eq!(
            plan.layers[0].record_spans,
            [
                RecordSpan {
                    begin_offset: 0,
                    end_offset: canvas_span.begin_offset,
                },
                RecordSpan {
                    begin_offset: run_end(0),
                    end_offset: run_end(1),
                },
            ]
        );
        assert_eq!(
            plan.layers[2].record_spans,
            [RecordSpan {
                begin_offset: canvas_span.end_offset,
                end_offset: run_end(0),
            }]
        );
    }

    fn layers_of_records(list: &TestList, plan: &LayerPlan) -> Vec<usize> {
        let mut layers = Vec::new();
        for_each_command(list.builder.bytes(), |_, record_offset, _| {
            layers.push(plan.layer_of_record(record_offset as u32));
        });
        layers
    }

    fn record_offsets(list: &TestList) -> Vec<u32> {
        let mut offsets = Vec::new();
        for_each_command(list.builder.bytes(), |_, record_offset, _| {
            offsets.push(record_offset as u32);
        });
        offsets.push(list.builder.bytes().len() as u32);
        offsets
    }

    #[test]
    fn records_in_the_run_of_a_canvas_are_placed_one_by_one() {
        let tree = identity_tree();
        let root = ContextRef::default();
        let list = TestList::new()
            .with(fill(IntRect::new(0, 0, 800, 600)), root)
            .with(canvas(IntRect::new(100, 100, 200, 100)), root)
            .with(fill(IntRect::new(150, 150, 20, 20)), root)
            .with(fill(IntRect::new(500, 400, 20, 20)), root)
            .with(fill(IntRect::new(160, 160, 20, 20)), root);
        assert_eq!(list.builder.command_runs().len(), 1);
        let plan = list.plan(&tree, &[]).unwrap();
        assert_eq!(kinds(&plan), ["base", "media", "painted"]);
        assert_eq!(layers_of_records(&list, &plan), [0, 1, 2, 0, 2]);
        assert_eq!(plan.layers[2].rect, IntRect::new(147, 147, 36, 36));
        let offsets = record_offsets(&list);
        let span = |first_record: usize, record_after_last: usize| RecordSpan {
            begin_offset: offsets[first_record],
            end_offset: offsets[record_after_last],
        };
        assert_eq!(plan.layers[0].record_spans, [span(0, 1), span(3, 4)]);
        assert_eq!(plan.layers[1].record_spans, [span(1, 2)]);
        assert_eq!(plan.layers[2].record_spans, [span(2, 3), span(4, 5)]);
    }

    #[test]
    fn records_in_a_run_that_touches_a_canvas_are_placed_one_by_one() {
        let mut tree = identity_tree();
        let root = ContextRef::default();
        let boxes = separate_clip(&mut tree, root);
        let list = TestList::new()
            .with(canvas(IntRect::new(100, 100, 200, 100)), root)
            .with(fill(IntRect::new(500, 400, 20, 20)), boxes)
            .with(fill(IntRect::new(150, 150, 20, 20)), boxes)
            .with(fill(IntRect::new(600, 400, 20, 20)), boxes);
        assert_eq!(list.builder.command_runs().len(), 2);
        let plan = list.plan(&tree, &[]).unwrap();
        assert_eq!(layers_of_records(&list, &plan), [1, 0, 2, 0]);
    }

    #[test]
    fn a_run_under_a_perspective_transform_is_placed_whole() {
        let mut tree = identity_tree();
        let mut perspective_matrix = FloatMatrix4x4::identity();
        perspective_matrix.elements[3][0] = 0.0001;
        let under_perspective = tree.append_spatial_under(
            ContextRef::default(),
            SpatialData::Transform(TransformData {
                matrix: perspective_matrix,
                origin: FloatPoint::default(),
                sorting_context_root_index: None,
                flattens_inherited_transform: false,
                role: TransformDataRole::CssTransform,
                synthetic_plane: false,
                establishes_sorting_context: false,
                bounds: FloatRect::default(),
            }),
        );
        let list = TestList::new()
            .with(canvas(IntRect::new(100, 100, 200, 100)), ContextRef::default())
            .with(fill(IntRect::new(150, 150, 20, 20)), under_perspective)
            .with(fill(IntRect::new(500, 400, 20, 20)), under_perspective);
        let plan = list.plan(&tree, &[]).unwrap();
        assert_eq!(layers_of_records(&list, &plan), [1, 2, 2]);
    }

    #[test]
    fn compositor_metadata_stays_in_the_layer_of_the_record_before_it() {
        let tree = identity_tree();
        let root = ContextRef::default();
        let metadata = CompositorBlockingWheelEventRegion {
            rect: FloatRect::new(0.0, 0.0, 900.0, 900.0),
        };
        let list = TestList::new()
            .with(canvas(IntRect::new(100, 100, 200, 100)), root)
            .with(fill(IntRect::new(150, 150, 20, 20)), root)
            .with(metadata, root)
            .with(fill(IntRect::new(160, 160, 20, 20)), root);
        let plan = list.plan(&tree, &[]).unwrap();
        assert_eq!(layers_of_records(&list, &plan), [1, 2, 2, 2]);
        assert_eq!(plan.layers[2].record_spans.len(), 1);
    }

    #[test]
    fn a_canvas_under_a_group_effect_is_not_a_layer() {
        let mut tree = tree_with_root_isolation_effect();
        let root = ContextRef {
            effect: tree.root_isolation_effect.unwrap(),
            ..ContextRef::default()
        };
        let group = tree.append_effect_node_under(root, effects(0.5, CompositingAndBlendingOperator::Normal));
        let list = TestList::new().with(canvas(IntRect::new(100, 100, 200, 100)), group);
        assert_eq!(
            list.plan(&tree, &[]),
            Err(UnlayeredReason::NoMediaCanBeLayer {
                first_rejected_media: CANVAS,
                rejection: MediaLayerRejection::InsideGroupEffect,
            })
        );
    }

    #[test]
    fn a_canvas_between_the_runs_of_one_group_is_not_a_layer() {
        let mut tree = tree_with_root_isolation_effect();
        let root = ContextRef {
            effect: tree.root_isolation_effect.unwrap(),
            ..ContextRef::default()
        };
        let group = tree.append_effect_node_under(root, effects(1.0, CompositingAndBlendingOperator::Normal));
        let list = TestList::new()
            .with(fill(IntRect::new(0, 0, 10, 10)), group)
            .with(canvas(IntRect::new(100, 100, 200, 100)), root)
            .with(fill(IntRect::new(20, 0, 10, 10)), group);
        assert_eq!(
            list.plan(&tree, &[]),
            Err(UnlayeredReason::NoMediaCanBeLayer {
                first_rejected_media: CANVAS,
                rejection: MediaLayerRejection::BetweenRunsOfOneGroupEffect,
            })
        );
    }

    #[test]
    fn a_blend_mode_reading_the_root_group_leaves_the_list_unlayered() {
        let mut tree = tree_with_root_isolation_effect();
        let root = ContextRef {
            effect: tree.root_isolation_effect.unwrap(),
            ..ContextRef::default()
        };
        let multiply = tree.append_effect_node_under(root, effects(1.0, CompositingAndBlendingOperator::Multiply));
        let list = TestList::new()
            .with(canvas(IntRect::new(100, 100, 200, 100)), root)
            .with(fill(IntRect::new(500, 400, 20, 20)), multiply);
        assert_eq!(list.plan(&tree, &[]), Err(UnlayeredReason::BlendModeReadsRootGroup));
    }

    #[test]
    fn a_canvas_that_does_not_update_is_not_a_layer() {
        let tree = identity_tree();
        let list = TestList::new().with(canvas(IntRect::new(100, 100, 200, 100)), ContextRef::default());
        let (tape, runs) = (list.builder.bytes(), list.builder.command_runs());
        let summary = summarize(tape, runs);
        let effect_clips = EffectClipPlan::new(&tree, runs).unwrap();
        let plan = build_layer_plan(&LayerPlanInputs {
            tape,
            command_runs: runs,
            summary: &summary,
            effect_clips: &effect_clips,
            tree: &tree,
            scroll_offsets: &[],
            viewport_rect: VIEWPORT,
            active_media: &[],
        });
        assert_eq!(plan, Err(UnlayeredReason::NoActiveMedia));
    }

    fn list_with_scrolled_box(tree: &mut VisualContextTree) -> (TestList, SpatialNodeIndex) {
        let scroll_node = tree.append_spatial(
            SpatialData::Scroll(ScrollData {
                state_slot: crate::visual_context::scroll_state::NO_SCROLL_STATE_SLOT,
                owner_paintable: NodeSlotId::INVALID,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let scrolled = ContextRef {
            spatial: scroll_node,
            ..ContextRef::default()
        };
        let list = TestList::new()
            .with(canvas(IntRect::new(100, 100, 200, 100)), ContextRef::default())
            .with(fill(IntRect::new(500, 120, 20, 20)), scrolled);
        (list, scroll_node)
    }

    #[test]
    fn scrolling_content_over_a_canvas_moves_it_above_the_canvas() {
        let mut tree = identity_tree();
        let (list, scroll_node) = list_with_scrolled_box(&mut tree);
        let mut scroll_offsets = vec![FloatPoint::default(); tree.spatial_nodes.len()];
        let unscrolled = list.plan(&tree, &scroll_offsets).unwrap();
        assert_eq!(kinds(&unscrolled), ["base", "media"]);
        scroll_offsets[scroll_node.0 as usize] = FloatPoint { x: -300.0, y: 0.0 };
        let scrolled = list.plan(&tree, &scroll_offsets).unwrap();
        assert_eq!(kinds(&scrolled), ["base", "media", "painted"]);
        assert!(!scrolled.has_same_layers_as(&unscrolled));
    }

    #[test]
    fn content_scrolled_within_its_layer_damages_only_that_layer() {
        let mut tree = identity_tree();
        let (list, scroll_node) = list_with_scrolled_box(&mut tree);
        let unscrolled_offsets = vec![FloatPoint::default(); tree.spatial_nodes.len()];
        let mut scrolled_offsets = unscrolled_offsets.clone();
        scrolled_offsets[scroll_node.0 as usize] = FloatPoint { x: 0.0, y: 50.0 };
        let before = list.plan(&tree, &unscrolled_offsets).unwrap();
        let after = list.plan(&tree, &scrolled_offsets).unwrap();
        assert!(after.has_same_layers_as(&before));
        let damage = damage_with_layer_plans(
            &tree,
            (&list, &before, &unscrolled_offsets),
            (&list, &after, &scrolled_offsets),
        );
        assert_eq!(damage.layer_rects, [damage.rect, Some(IntRect::default())]);
    }

    struct ContextsAroundACanvas {
        under_box: ContextRef,
        canvas: ContextRef,
        boxes_after_canvas: Vec<ContextRef>,
    }

    // Each box gets a clip of its own, so that it is a run of its own.
    fn contexts_around_a_canvas(tree: &mut VisualContextTree, box_count_after_canvas: usize) -> ContextsAroundACanvas {
        let root = ContextRef {
            effect: tree.root_isolation_effect.unwrap(),
            ..ContextRef::default()
        };
        ContextsAroundACanvas {
            under_box: separate_clip(tree, root),
            canvas: root,
            boxes_after_canvas: (0..box_count_after_canvas).map(|_| separate_clip(tree, root)).collect(),
        }
    }

    fn list_around_a_canvas(
        contexts: &ContextsAroundACanvas,
        (under, under_color): (IntRect, Color),
        boxes_after_canvas: &[IntRect],
    ) -> TestList {
        let mut list = TestList::new()
            .with(
                FillRect {
                    color: under_color,
                    ..fill(under)
                },
                contexts.under_box,
            )
            .with(canvas(IntRect::new(100, 100, 200, 100)), contexts.canvas);
        for (rect, context) in boxes_after_canvas.iter().zip(&contexts.boxes_after_canvas) {
            list = list.with(fill(*rect), *context);
        }
        list
    }

    #[test]
    fn content_that_changes_under_a_canvas_damages_only_the_base_layer() {
        let mut tree = tree_with_root_isolation_effect();
        let contexts = contexts_around_a_canvas(&mut tree, 2);
        let list_with_under_color = |color| {
            list_around_a_canvas(
                &contexts,
                (IntRect::new(50, 50, 400, 300), color),
                &[IntRect::new(150, 150, 20, 20), IntRect::new(600, 500, 20, 20)],
            )
        };
        let old_list = list_with_under_color(Color::from_rgb(255, 0, 0));
        let new_list = list_with_under_color(Color::from_rgb(0, 0, 255));
        let old_plan = old_list.plan(&tree, &[]).unwrap();
        let new_plan = new_list.plan(&tree, &[]).unwrap();
        assert_eq!(kinds(&new_plan), ["base", "media", "painted"]);
        let damage = damage_with_layer_plans(&tree, (&old_list, &old_plan, &[]), (&new_list, &new_plan, &[]));
        let under_rect = Some(IntRect::new(49, 49, 402, 302));
        assert_eq!(damage.rect, under_rect);
        assert_eq!(
            damage.layer_rects,
            [under_rect, Some(IntRect::default()), Some(IntRect::default())]
        );
    }

    const SMALL_RED_BOX: (IntRect, Color) = (IntRect::new(0, 0, 10, 10), Color::from_rgb(255, 0, 0));

    #[test]
    fn a_box_that_moves_off_a_canvas_damages_the_layer_it_leaves_and_the_one_it_enters() {
        let mut tree = tree_with_root_isolation_effect();
        let contexts = contexts_around_a_canvas(&mut tree, 2);
        let list_with_second_box_at =
            |second| list_around_a_canvas(&contexts, SMALL_RED_BOX, &[IntRect::new(150, 150, 20, 20), second]);
        let old_list = list_with_second_box_at(IntRect::new(160, 160, 20, 20));
        let new_list = list_with_second_box_at(IntRect::new(600, 500, 20, 20));
        let old_plan = old_list.plan(&tree, &[]).unwrap();
        let new_plan = new_list.plan(&tree, &[]).unwrap();
        assert_eq!(kinds(&old_plan), ["base", "media", "painted"]);
        assert!(new_plan.has_same_layers_as(&old_plan));
        let damage = damage_with_layer_plans(&tree, (&old_list, &old_plan, &[]), (&new_list, &new_plan, &[]));
        assert_eq!(
            damage.layer_rects,
            [
                Some(IntRect::new(599, 499, 22, 22)),
                Some(IntRect::default()),
                Some(IntRect::new(159, 159, 22, 22)),
            ]
        );
    }

    // The first box grows over an unchanged box, which the layer above the canvas then takes in. With `last_box`, a
    // box after the unchanged one changes too, so that the diff compares the unchanged box command by command.
    fn assert_unchanged_box_taken_in_by_a_growing_layer_damages_both_layers(last_box: Option<(IntRect, IntRect)>) {
        let mut tree = tree_with_root_isolation_effect();
        let contexts = contexts_around_a_canvas(&mut tree, 3);
        let unchanged_box = IntRect::new(400, 150, 20, 20);
        let list_with = |first_box, last_box: Option<IntRect>| {
            let mut boxes = vec![first_box, unchanged_box];
            boxes.extend(last_box);
            list_around_a_canvas(&contexts, SMALL_RED_BOX, &boxes)
        };
        let old_list = list_with(IntRect::new(150, 150, 20, 20), last_box.map(|(old, _)| old));
        let new_list = list_with(IntRect::new(150, 150, 300, 20), last_box.map(|(_, new)| new));
        let old_plan = old_list.plan(&tree, &[]).unwrap();
        let new_plan = new_list.plan(&tree, &[]).unwrap();
        let unchanged_box_record = new_list.builder.command_runs()[3].offset;
        assert_eq!(old_plan.layer_of_record(unchanged_box_record), 0);
        assert_eq!(new_plan.layer_of_record(unchanged_box_record), 2);
        let damage = damage_with_layer_plans(&tree, (&old_list, &old_plan, &[]), (&new_list, &new_plan, &[]));
        assert!(damage.layer_rects[0].unwrap().contains_rect(unchanged_box));
        assert!(damage.layer_rects[2].unwrap().contains_rect(unchanged_box));
    }

    #[test]
    fn an_unchanged_box_that_a_growing_layer_takes_in_damages_the_layer_it_leaves() {
        assert_unchanged_box_taken_in_by_a_growing_layer_damages_both_layers(None);
    }

    #[test]
    fn an_unchanged_box_between_changed_boxes_that_a_growing_layer_takes_in_damages_the_layer_it_leaves() {
        assert_unchanged_box_taken_in_by_a_growing_layer_damages_both_layers(Some((
            IntRect::new(600, 500, 20, 20),
            IntRect::new(620, 500, 20, 20),
        )));
    }

    #[test]
    fn an_unchanged_run_that_a_growing_layer_splits_differently_damages_the_layers_its_record_moves_between() {
        let mut tree = identity_tree();
        let root = ContextRef::default();
        let growing_box = separate_clip(&mut tree, root);
        let unchanged_box = IntRect::new(400, 150, 20, 20);
        let list_with_growing_box = |growing_box_rect| {
            TestList::new()
                .with(canvas(IntRect::new(100, 100, 200, 100)), root)
                .with(fill(growing_box_rect), growing_box)
                .with(fill(unchanged_box), root)
                .with(fill(IntRect::new(160, 160, 20, 20)), root)
        };
        let old_list = list_with_growing_box(IntRect::new(150, 150, 20, 20));
        let new_list = list_with_growing_box(IntRect::new(150, 150, 300, 20));
        let old_plan = old_list.plan(&tree, &[]).unwrap();
        let new_plan = new_list.plan(&tree, &[]).unwrap();
        assert_eq!(layers_of_records(&old_list, &old_plan), [1, 2, 0, 2]);
        assert_eq!(layers_of_records(&new_list, &new_plan), [1, 2, 2, 2]);
        assert!(new_plan.has_same_layers_as(&old_plan));
        let damage = damage_with_layer_plans(&tree, (&old_list, &old_plan, &[]), (&new_list, &new_plan, &[]));
        assert!(damage.layer_rects[0].unwrap().contains_rect(unchanged_box));
        assert!(damage.layer_rects[2].unwrap().contains_rect(unchanged_box));
    }

    fn placement_of_only_media_layer(plan: &LayerPlan) -> Result<WindowServerPlacement, WindowServerRejection> {
        let media_layers: Vec<&Layer> = plan
            .layers
            .iter()
            .filter(|layer| matches!(layer.kind, LayerKind::Media { .. }))
            .collect();
        assert_eq!(media_layers.len(), 1);
        media_layers[0].window_server_placement.unwrap()
    }

    fn rounded_rect_clip(rect: FloatRect, corner_radii: CornerRadii) -> ClipNodeData {
        ClipNodeData::Rect(crate::visual_context::ClipData {
            rect,
            corner_radii,
            mode: ClipMode::Intersect,
        })
    }

    fn transform(matrix: FloatMatrix4x4) -> SpatialData {
        SpatialData::Transform(TransformData {
            matrix,
            origin: FloatPoint::default(),
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
            bounds: FloatRect::default(),
        })
    }

    #[test]
    fn rect_clips_of_a_canvas_intersect_into_one_clip_for_the_window_server() {
        let mut tree = identity_tree();
        let clip = tree.append_clip(
            ClipNodeData::rect_clip(FloatRect::new(50.0, 50.0, 200.0, 300.0)),
            ClipNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let clipped = ContextRef {
            clip,
            ..ContextRef::default()
        };
        let list = TestList::new().with_inline_clips(
            canvas(IntRect::new(100, 100, 200, 100)),
            clipped,
            &[PendingInlineClip::intersecting_float_rect(FloatRect::new(
                0.0, 0.0, 220.0, 150.0,
            ))],
        );
        let placement = placement_of_only_media_layer(&list.plan(&tree, &[]).unwrap()).unwrap();
        assert_eq!(placement.content_rect, IntRect::new(100, 100, 200, 100));
        assert!(placement.content_to_viewport.is_identity());
        assert_eq!(placement.clip_rect, FloatRect::new(50.0, 50.0, 170.0, 100.0));
        assert_eq!(placement.rounded_clip, None);
        assert_eq!(placement.scaling_mode, ScalingMode::NearestNeighbor);
    }

    #[test]
    fn a_rounded_clip_whose_corners_miss_the_content_clips_it_as_a_rect() {
        let mut tree = identity_tree();
        let clip = tree.append_clip(
            rounded_rect_clip(FloatRect::new(10.0, 10.0, 320.0, 240.0), CornerRadii::uniform(16)),
            ClipNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        // The clip is in another space than the content, as a scroller's is, and the corners are above and below the
        // letterboxed picture.
        let scrolled = tree.append_spatial_under(
            ContextRef {
                clip,
                ..ContextRef::default()
            },
            transform(FloatMatrix4x4::identity()),
        );
        let list = TestList::new().with(canvas(IntRect::new(10, 40, 320, 180)), scrolled);
        let placement = placement_of_only_media_layer(&list.plan(&tree, &[]).unwrap()).unwrap();
        assert_eq!(placement.clip_rect, FloatRect::new(10.0, 10.0, 320.0, 240.0));
        assert_eq!(placement.rounded_clip, None);
    }

    #[test]
    fn a_rounded_clip_that_cuts_the_content_keeps_its_radius_and_corners() {
        let mut tree = identity_tree();
        let mut corner_radii = CornerRadii::uniform(8);
        corner_radii.bottom_left = Default::default();
        let scaled = tree.append_spatial_under(ContextRef::default(), transform(scale_matrix(2.0, 2.0, 1.0)));
        let list = TestList::new().with_inline_clips(
            canvas(IntRect::new(10, 10, 100, 50)),
            scaled,
            &[PendingInlineClip::intersecting_rounded_rect(
                FloatRect::new(10.0, 10.0, 100.0, 50.0),
                corner_radii,
            )],
        );
        let placement = placement_of_only_media_layer(&list.plan(&tree, &[]).unwrap()).unwrap();
        assert_eq!(placement.clip_rect, FloatRect::new(20.0, 20.0, 200.0, 100.0));
        let rounded_clip = placement.rounded_clip.unwrap();
        assert_eq!(rounded_clip.rect, FloatRect::new(20.0, 20.0, 200.0, 100.0));
        assert_eq!(rounded_clip.radius, 16.0);
        assert_eq!(
            rounded_clip.rounded_corners,
            crate::display_list::window_server_placement::RoundedCorners {
                top_left: true,
                top_right: true,
                bottom_right: true,
                bottom_left: false,
            }
        );
    }

    #[test]
    fn a_rotated_canvas_under_a_clip_of_an_unrotated_box_is_shown_by_the_window_server() {
        let mut tree = identity_tree();
        let clip = tree.append_clip(
            ClipNodeData::rect_clip(FloatRect::new(0.0, 0.0, 400.0, 300.0)),
            ClipNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let rotated = tree.append_spatial_under(
            ContextRef {
                clip,
                ..ContextRef::default()
            },
            transform(rotation_matrix([0.0, 0.0, 1.0], std::f32::consts::FRAC_PI_2)),
        );
        let list = TestList::new().with(canvas(IntRect::new(100, -300, 200, 100)), rotated);
        let placement = placement_of_only_media_layer(&list.plan(&tree, &[]).unwrap()).unwrap();
        let [a, b, c, d, _, _] = placement.content_to_viewport.values;
        assert!(a.abs() < 1e-6 && (b - 1.0).abs() < 1e-6 && (c + 1.0).abs() < 1e-6 && d.abs() < 1e-6);
        assert_eq!(placement.clip_rect, FloatRect::new(0.0, 0.0, 400.0, 300.0));
    }

    #[test]
    fn a_clip_in_the_rotated_space_of_a_video_that_leaves_it_whole_needs_no_layer() {
        let mut tree = identity_tree();
        let rotated =
            tree.append_spatial_under(ContextRef::default(), transform(rotation_matrix([0.0, 0.0, 1.0], 0.3)));
        let list = TestList::new().with_inline_clips(
            canvas(IntRect::new(100, 100, 200, 100)),
            rotated,
            &[PendingInlineClip::intersecting_rounded_rect(
                FloatRect::new(100.0, 80.0, 200.0, 140.0),
                CornerRadii::uniform(10),
            )],
        );
        let placement = placement_of_only_media_layer(&list.plan(&tree, &[]).unwrap()).unwrap();
        assert_eq!(placement.clip_rect, VIEWPORT.to_float());
        assert_eq!(placement.rounded_clip, None);
    }

    #[test]
    fn a_clip_around_a_canvas_that_its_inline_transform_moves_out_of_stays() {
        let tree = identity_tree();
        let mut builder = DisplayListBuilder::new();
        builder.append_with_inline_state(
            &canvas(IntRect::new(0, 0, 100, 100)),
            &[],
            ContextRef::default(),
            &[PendingInlineClip::intersecting_float_rect(FloatRect::new(
                0.0, 0.0, 100.0, 100.0,
            ))],
            Some(AffineTransform::identity().translated(20.0, 0.0)),
        );
        let list = TestList { builder };
        let placement = placement_of_only_media_layer(&list.plan(&tree, &[]).unwrap()).unwrap();
        assert_eq!(placement.clip_rect, FloatRect::new(0.0, 0.0, 100.0, 100.0));
        assert_eq!(placement.content_to_viewport.values, [1.0, 0.0, 0.0, 1.0, 20.0, 0.0]);
    }

    #[test]
    fn media_layers_that_the_window_server_cannot_clip_are_replayed() {
        let rejection_for = |tree: &VisualContextTree, context: ContextRef, inline_clips: &[PendingInlineClip]| {
            let list =
                TestList::new().with_inline_clips(canvas(IntRect::new(100, 100, 200, 100)), context, inline_clips);
            placement_of_only_media_layer(&list.plan(tree, &[]).unwrap()).unwrap_err()
        };
        let tree = identity_tree();
        let root = ContextRef::default();
        let content = FloatRect::new(100.0, 100.0, 200.0, 100.0);

        let mut triangle = libgfx_rust::path::Path::new();
        triangle.move_to(FloatPoint { x: 100.0, y: 100.0 });
        triangle.line_to(FloatPoint { x: 300.0, y: 100.0 });
        triangle.line_to(FloatPoint { x: 100.0, y: 200.0 });
        triangle.close();
        assert_eq!(
            rejection_for(
                &tree,
                root,
                &[PendingInlineClip::intersecting_path(
                    &triangle,
                    libgfx_rust::WindingRule::Nonzero
                )]
            ),
            WindowServerRejection::ClippedByPath
        );
        assert_eq!(
            rejection_for(
                &tree,
                root,
                &[PendingInlineClip::subtracting_rect(FloatRect::new(
                    150.0, 120.0, 20.0, 20.0
                ))]
            ),
            WindowServerRejection::ClippedByExcludingClip
        );

        let mut differing_radii = CornerRadii::uniform(8);
        differing_radii.top_left = libgfx_rust::CornerRadius {
            horizontal_radius: 12,
            vertical_radius: 12,
        };
        assert_eq!(
            rejection_for(
                &tree,
                root,
                &[PendingInlineClip::intersecting_rounded_rect(content, differing_radii)]
            ),
            WindowServerRejection::RoundedCornersOfDifferingRadii
        );
        let mut elliptical_radii = CornerRadii::uniform(8);
        elliptical_radii.bottom_right.vertical_radius = 4;
        assert_eq!(
            rejection_for(
                &tree,
                root,
                &[PendingInlineClip::intersecting_rounded_rect(content, elliptical_radii)]
            ),
            WindowServerRejection::RoundedCornersOfDifferingRadii
        );
        assert_eq!(
            rejection_for(
                &tree,
                root,
                &[
                    PendingInlineClip::intersecting_rounded_rect(content, CornerRadii::uniform(8)),
                    PendingInlineClip::intersecting_rounded_rect(
                        FloatRect::new(90.0, 90.0, 220.0, 120.0),
                        CornerRadii::uniform(30)
                    ),
                ]
            ),
            WindowServerRejection::MoreThanOneRoundedClip
        );

        let mut tree_with_rotated_clip = identity_tree();
        let rotated =
            tree_with_rotated_clip.append_spatial_under(root, transform(rotation_matrix([0.0, 0.0, 1.0], 0.3)));
        let clip = tree_with_rotated_clip.append_clip(
            ClipNodeData::rect_clip(FloatRect::new(0.0, 0.0, 150.0, 300.0)),
            ClipNodeIndex::NONE,
            rotated.spatial,
        );
        assert_eq!(
            rejection_for(&tree_with_rotated_clip, ContextRef { clip, ..rotated }, &[]),
            WindowServerRejection::ClippedInRotatedOrSkewedSpace
        );

        let mut tree_with_rounded_clip_node = identity_tree();
        let rounded = tree_with_rounded_clip_node.append_clip(
            rounded_rect_clip(content, CornerRadii::uniform(10)),
            ClipNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        assert_eq!(
            rejection_for(
                &tree_with_rounded_clip_node,
                ContextRef { clip: rounded, ..root },
                &[PendingInlineClip::intersecting_rounded_rect(
                    content,
                    CornerRadii::uniform(10)
                )]
            ),
            WindowServerRejection::MoreThanOneRoundedClip
        );
    }
}
