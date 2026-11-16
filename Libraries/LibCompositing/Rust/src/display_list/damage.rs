/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::chunks::{ReachOutsideChunk, TapeChunks, TopLevelRecordReachingOutsideItsChunk};
use super::effect_clip_plan::EffectClipPlan;
use super::effect_geometry::{Extent, changed_extent_through_filters, nearest_spreading_effects};
use super::layers::{LayerKind, LayerPlan};
use super::replay::{OwnSpaceClipExtent, own_space_clip_extent};
use super::summary::{DisplayListSummary, LiveContent};
use crate::display_list::builder::{
    HEADER_SIZE, bounding_rect_computed_from_payload, for_each_command, has_inline_clip_in_its_own_space,
    inline_clips_of, read_header,
};
use crate::display_list::commands::{
    CanvasId, ClipMode, ClipNodeIndex, CompositorContextId, ContextRef, DisplayListCommandHeader,
    DisplayListCommandRun, EffectNodeIndex, ImageFrameResourceId, SpatialNodeIndex, VISUAL_VIEWPORT_NODE_INDEX,
    VideoSinkHandle,
};
use crate::host::FfiAnimatedContentViewportEffect;
use crate::visual_context::queries::TreeCullingScratch;
use crate::visual_context::{
    ClipNodeData, EffectNodeData, SpatialData, VisualContextTree, WorldTransforms, device_offset_for_index,
};
use libgfx_rust::{FloatMatrix4x4, FloatPoint, FloatRect, IntRect, enclosing_int_rect};
use std::cell::OnceCell;

struct RecordOnTape<'a> {
    header: DisplayListCommandHeader,
    payload: &'a [u8],
    context: ContextRef,
    record_offset: u32,
}

impl<'a> RecordOnTape<'a> {
    fn at(bytes: &'a [u8], record_offset: u32, context: ContextRef) -> Self {
        let offset = record_offset as usize;
        let header = read_header(&bytes[offset..]);
        let payload_start = offset + HEADER_SIZE;
        Self {
            header,
            payload: &bytes[payload_start..payload_start + header.payload_size as usize],
            context,
            record_offset,
        }
    }
}

fn spatial_data_is_equal(
    index: SpatialNodeIndex,
    a: &SpatialData,
    a_scroll_offsets: &[FloatPoint],
    b: &SpatialData,
    b_scroll_offsets: &[FloatPoint],
) -> bool {
    match (a, b) {
        // The payload carries no offset key; replay reads the offset stored under each node's
        // own index, so that is what has to match.
        (SpatialData::Scroll(_), SpatialData::Scroll(_)) | (SpatialData::Sticky(_), SpatialData::Sticky(_)) => {
            device_offset_for_index(a_scroll_offsets, index) == device_offset_for_index(b_scroll_offsets, index)
        }
        (SpatialData::Transform(data), SpatialData::Transform(other)) => data == other,
        (SpatialData::Perspective(data), SpatialData::Perspective(other)) => data == other,
        (SpatialData::BackfaceVisibility(data), SpatialData::BackfaceVisibility(other)) => data == other,
        (SpatialData::AnchorScrollShift(data), SpatialData::AnchorScrollShift(other)) => {
            data.negate == other.negate
                && data.compensate_horizontal_scroll == other.compensate_horizontal_scroll
                && data.compensate_vertical_scroll == other.compensate_vertical_scroll
                && data.masked_offset(a_scroll_offsets) == other.masked_offset(b_scroll_offsets)
        }
        _ => false,
    }
}

fn clip_data_is_equal(a: &ClipNodeData, b: &ClipNodeData) -> bool {
    match (a, b) {
        (ClipNodeData::Rect(data), ClipNodeData::Rect(other)) => data == other,
        (ClipNodeData::Path(data), ClipNodeData::Path(other)) => {
            data.bounding_rect == other.bounding_rect
                && data.fill_rule == other.fill_rule
                && (std::sync::Arc::ptr_eq(&data.path, &other.path) || *data.path == *other.path)
        }
        _ => false,
    }
}

fn effect_data_is_equal(a: &EffectNodeData, b: &EffectNodeData) -> bool {
    match (a, b) {
        (EffectNodeData::BackgroundColorAnimation, EffectNodeData::BackgroundColorAnimation) => true,
        (EffectNodeData::Effects(data), EffectNodeData::Effects(other)) => {
            data.isolation == other.isolation
                && data.opacity == other.opacity
                && data.blend_mode == other.blend_mode
                && data.filter == other.filter
                && data.backdrop_filter == other.backdrop_filter
        }
        (EffectNodeData::Mask(data), EffectNodeData::Mask(other)) => data == other,
        _ => false,
    }
}

fn propagate_changes_to_descendants(changed: &mut [bool], parent_of: impl Fn(usize) -> Option<usize>) {
    if !changed.contains(&true) {
        return;
    }
    let node_count = changed.len();
    let mut verdicts: Vec<Option<bool>> = changed.iter().map(|&changed| changed.then_some(true)).collect();
    let mut unresolved_ancestors = Vec::new();
    for start in 0..node_count {
        let mut node = start;
        let chain_changed = loop {
            if let Some(verdict) = verdicts[node] {
                break verdict;
            }
            if unresolved_ancestors.len() > node_count {
                break true;
            }
            unresolved_ancestors.push(node);
            match parent_of(node) {
                None => break false,
                Some(parent) if parent < node_count => node = parent,
                Some(_) => break true,
            }
        };
        for node in unresolved_ancestors.drain(..) {
            verdicts[node] = Some(chain_changed);
        }
    }
    for (changed, verdict) in changed.iter_mut().zip(verdicts) {
        *changed = verdict.unwrap_or(true);
    }
}

#[derive(Default)]
struct ChangedVisualContextNodes {
    spatial: Vec<bool>,
    clip: Vec<bool>,
    effect: Vec<bool>,
    any_changed: bool,
}

fn changed_spatial_nodes(
    (old_tree, old_scroll_offsets): (&VisualContextTree, &[FloatPoint]),
    (new_tree, new_scroll_offsets): (&VisualContextTree, &[FloatPoint]),
) -> Vec<bool> {
    let mut changed: Vec<bool> = (0..old_tree.spatial_nodes.len().max(new_tree.spatial_nodes.len()))
        .map(|index| {
            let (Some(old_node), Some(new_node)) =
                (old_tree.spatial_nodes.get(index), new_tree.spatial_nodes.get(index))
            else {
                return true;
            };
            old_node.parent != new_node.parent
                || !spatial_data_is_equal(
                    SpatialNodeIndex(index as u32),
                    &old_node.data,
                    old_scroll_offsets,
                    &new_node.data,
                    new_scroll_offsets,
                )
        })
        .collect();
    propagate_changes_to_descendants(&mut changed, |index| {
        (index != VISUAL_VIEWPORT_NODE_INDEX.0 as usize).then(|| new_tree.spatial_nodes[index].parent.0 as usize)
    });
    changed
}

fn changed_clip_nodes(
    old_tree: &VisualContextTree,
    new_tree: &VisualContextTree,
    changed_spatial_nodes: &[bool],
) -> Vec<bool> {
    let mut changed: Vec<bool> = (0..old_tree.clip_nodes.len().max(new_tree.clip_nodes.len()))
        .map(|index| {
            let (Some(old_node), Some(new_node)) = (old_tree.clip_nodes.get(index), new_tree.clip_nodes.get(index))
            else {
                return true;
            };
            old_node.parent != new_node.parent
                || old_node.spatial != new_node.spatial
                || !clip_data_is_equal(&old_node.data, &new_node.data)
                || changed_spatial_nodes
                    .get(new_node.spatial.0 as usize)
                    .is_none_or(|changed| *changed)
        })
        .collect();
    propagate_changes_to_descendants(&mut changed, |index| {
        let parent = new_tree.clip_nodes[index].parent;
        (!parent.is_none()).then_some(parent.0 as usize)
    });
    changed
}

fn effect_nodes_that_differ_where_they_are(
    (old_tree, old_effect_clips): (&VisualContextTree, &EffectClipPlan),
    (new_tree, new_effect_clips): (&VisualContextTree, &EffectClipPlan),
    changed_spatial_nodes: &[bool],
    changed_clip_nodes: &[bool],
) -> Vec<bool> {
    let clip_changed = |clip: ClipNodeIndex| changed_clip_nodes.get(clip.0 as usize).copied().unwrap_or(false);
    (0..old_tree.effect_nodes.len().max(new_tree.effect_nodes.len()))
        .map(|index| {
            let (Some(old_node), Some(new_node)) = (old_tree.effect_nodes.get(index), new_tree.effect_nodes.get(index))
            else {
                return true;
            };
            let effect = EffectNodeIndex(index as u32);
            let new_output_clip = new_effect_clips.output_clip_if_participating(effect);
            old_node.parent != new_node.parent
                || old_node.spatial != new_node.spatial
                || old_node.local_clip != new_node.local_clip
                || old_effect_clips.output_clip_if_participating(effect) != new_output_clip
                || !effect_data_is_equal(&old_node.data, &new_node.data)
                || changed_spatial_nodes
                    .get(new_node.spatial.0 as usize)
                    .is_none_or(|changed| *changed)
                || clip_changed(new_node.local_clip)
                || new_output_clip.is_some_and(clip_changed)
        })
        .collect()
}

impl ChangedVisualContextNodes {
    fn spatial_changed(&self, index: SpatialNodeIndex) -> bool {
        self.spatial.get(index.0 as usize).copied().unwrap_or(false)
    }

    fn clip_changed(&self, index: ClipNodeIndex) -> bool {
        self.clip.get(index.0 as usize).copied().unwrap_or(false)
    }

    fn effect_changed(&self, index: EffectNodeIndex) -> bool {
        self.effect.get(index.0 as usize).copied().unwrap_or(false)
    }

    fn chains_changed(&self, context: ContextRef) -> bool {
        self.spatial_changed(context.spatial) || self.clip_changed(context.clip) || self.effect_changed(context.effect)
    }

    fn mark_effect_changed(&mut self, effect: EffectNodeIndex) {
        if let Some(changed) = self.effect.get_mut(effect.0 as usize) {
            *changed = true;
        }
    }

    fn between(
        old: &DiffSide<'_>,
        new: &DiffSide<'_>,
        scroll_offsets_differ: bool,
        pairings: impl Iterator<Item = ChunkPairing>,
    ) -> Self {
        let (old_tree, new_tree) = (old.geometry.tree, new.geometry.tree);
        let (old_effect_clips, new_effect_clips) = (old.geometry.effect_clips, new.geometry.effect_clips);
        let trees_draw_alike = std::ptr::eq(old_tree, new_tree)
            && !scroll_offsets_differ
            && (std::ptr::eq(old_effect_clips, new_effect_clips) || old_effect_clips == new_effect_clips);
        if trees_draw_alike && std::ptr::eq(old.frame.chunks, new.frame.chunks) {
            return Self::default();
        }

        let mut changed = Self::default();
        if trees_draw_alike {
            changed.effect = vec![false; new_tree.effect_nodes.len()];
        } else {
            changed.spatial = changed_spatial_nodes(
                (old_tree, old.geometry.scroll_offsets),
                (new_tree, new.geometry.scroll_offsets),
            );
            changed.clip = changed_clip_nodes(old_tree, new_tree, &changed.spatial);
            changed.effect = effect_nodes_that_differ_where_they_are(
                (old_tree, old_effect_clips),
                (new_tree, new_effect_clips),
                &changed.spatial,
                &changed.clip,
            );
        }
        for effect in changed.effects_whose_mask_content_changed(old, new, pairings) {
            changed.mark_effect_changed(effect);
        }
        if old_tree.root_isolation_effect != new_tree.root_isolation_effect {
            for effect in [old_tree.root_isolation_effect, new_tree.root_isolation_effect]
                .into_iter()
                .flatten()
            {
                changed.mark_effect_changed(effect);
            }
        }
        propagate_changes_to_descendants(&mut changed.effect, |index| {
            let parent = new_tree.effect_nodes.get(index)?.parent;
            (!parent.is_none()).then_some(parent.0 as usize)
        });

        changed.any_changed =
            changed.spatial.contains(&true) || changed.clip.contains(&true) || changed.effect.contains(&true);
        changed
    }

    fn effects_whose_mask_content_changed(
        &self,
        old: &DiffSide<'_>,
        new: &DiffSide<'_>,
        pairings: impl Iterator<Item = ChunkPairing>,
    ) -> Vec<EffectNodeIndex> {
        let declaration_moved = |run_index: u32| {
            let context = new.frame.command_runs[run_index as usize].context;
            self.spatial_changed(context.spatial) || self.clip_changed(context.clip)
        };
        let mut effects = Vec::new();
        for pairing in pairings {
            let (side, chunk, declaration_changed): (_, _, &dyn Fn(u32) -> bool) = match pairing {
                ChunkPairing::OnlyOld(chunk) => (old, chunk, &|_| true),
                ChunkPairing::OnlyNew(chunk) => (new, chunk, &|_| true),
                ChunkPairing::Matched { new: chunk, .. } => (new, chunk, &declaration_moved),
            };
            for record in side.frame.chunks.records_reaching_outside(chunk) {
                if let ReachOutsideChunk::DeclaresMaskContentOf(effect) = record.reach
                    && declaration_changed(record.run_index)
                {
                    effects.push(effect);
                }
            }
        }
        effects
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ChunkPairing {
    Matched { old: usize, new: usize },
    OnlyOld(usize),
    OnlyNew(usize),
}

fn chunk_positions_by_id(chunks: &TapeChunks, positions: std::ops::Range<usize>) -> Vec<([u64; 2], usize)> {
    let mut positions_by_id: Vec<([u64; 2], usize)> =
        positions.map(|position| (chunks.id(position).0, position)).collect();
    positions_by_id.sort_unstable();
    positions_by_id
}

fn next_position_of(positions_by_id: &[([u64; 2], usize)], id: [u64; 2], from: usize) -> Option<usize> {
    let first_at_or_after = positions_by_id.partition_point(|&entry| entry < (id, from));
    positions_by_id
        .get(first_at_or_after)
        .filter(|(other_id, _)| *other_id == id)
        .map(|&(_, position)| position)
}

fn pair_chunks_by_name(old: &TapeChunks, new: &TapeChunks) -> Vec<ChunkPairing> {
    let (old_count, new_count) = (old.len(), new.len());
    let mut pairings = Vec::with_capacity(old_count.max(new_count));
    let common_prefix_count = (0..old_count.min(new_count))
        .take_while(|&index| old.id(index) == new.id(index))
        .count();
    let common_suffix_count = (0..old_count.min(new_count) - common_prefix_count)
        .take_while(|&from_end| old.id(old_count - from_end - 1) == new.id(new_count - from_end - 1))
        .count();
    pairings.extend((0..common_prefix_count).map(|index| ChunkPairing::Matched { old: index, new: index }));

    let (old_middle_end, new_middle_end) = (old_count - common_suffix_count, new_count - common_suffix_count);
    let (mut old_index, mut new_index) = (common_prefix_count, common_prefix_count);
    if old_index < old_middle_end && new_index < new_middle_end {
        let old_positions_by_id = chunk_positions_by_id(old, old_index..old_middle_end);
        let new_positions_by_id = chunk_positions_by_id(new, new_index..new_middle_end);
        while old_index < old_middle_end && new_index < new_middle_end {
            let (old_id, new_id) = (old.id(old_index), new.id(new_index));
            if old_id == new_id {
                pairings.push(ChunkPairing::Matched {
                    old: old_index,
                    new: new_index,
                });
                old_index += 1;
                new_index += 1;
                continue;
            }
            match (
                next_position_of(&new_positions_by_id, old_id.0, new_index),
                next_position_of(&old_positions_by_id, new_id.0, old_index),
            ) {
                (Some(old_id_in_new), Some(new_id_in_old)) if old_id_in_new - new_index < new_id_in_old - old_index => {
                    pairings.push(ChunkPairing::OnlyNew(new_index));
                    new_index += 1;
                }
                (Some(_), None) => {
                    pairings.push(ChunkPairing::OnlyNew(new_index));
                    new_index += 1;
                }
                (_, Some(_)) => {
                    pairings.push(ChunkPairing::OnlyOld(old_index));
                    old_index += 1;
                }
                (None, None) => {
                    pairings.push(ChunkPairing::OnlyOld(old_index));
                    pairings.push(ChunkPairing::OnlyNew(new_index));
                    old_index += 1;
                    new_index += 1;
                }
            }
        }
    }
    pairings.extend((old_index..old_middle_end).map(ChunkPairing::OnlyOld));
    pairings.extend((new_index..new_middle_end).map(ChunkPairing::OnlyNew));

    pairings.extend((0..common_suffix_count).map(|from_start| ChunkPairing::Matched {
        old: old_middle_end + from_start,
        new: new_middle_end + from_start,
    }));
    pairings
}

struct SideGeometry<'a> {
    tree: &'a VisualContextTree,
    scroll_offsets: &'a [FloatPoint],
    effect_clips: &'a EffectClipPlan,
    culling: OnceCell<TreeCullingScratch>,
    nearest_spreading_effects: OnceCell<Vec<EffectNodeIndex>>,
    world_transforms: OnceCell<WorldTransforms>,
}

impl<'a> SideGeometry<'a> {
    fn new(tree: &'a VisualContextTree, scroll_offsets: &'a [FloatPoint], effect_clips: &'a EffectClipPlan) -> Self {
        Self {
            tree,
            scroll_offsets,
            effect_clips,
            culling: OnceCell::new(),
            nearest_spreading_effects: OnceCell::new(),
            world_transforms: OnceCell::new(),
        }
    }

    // Damage maps commands with the matrices replay draws them with.
    fn world(&self) -> &WorldTransforms {
        self.world_transforms
            .get_or_init(|| WorldTransforms::new(self.tree, self.scroll_offsets, Some(FloatMatrix4x4::identity())))
    }

    fn culling(&self) -> &TreeCullingScratch {
        self.culling.get_or_init(|| {
            let mut culling = TreeCullingScratch::default();
            self.tree.fill_culling_scratch(&mut culling);
            culling
        })
    }

    fn nearest_spreading_effects(&self) -> &[EffectNodeIndex] {
        self.nearest_spreading_effects
            .get_or_init(|| nearest_spreading_effects(self.tree))
    }

    // Where a command's inline clips in spaces of their own let it draw on one side of the comparison.
    fn own_space_clip_extent_of(&self, record: &RecordOnTape<'_>) -> Option<OwnSpaceClipExtent> {
        if record.header.inline_clip_count == 0 || !has_inline_clip_in_its_own_space(&record.header, record.payload) {
            return None;
        }
        let bounding_rect = bounding_rect_computed_from_payload(&record.header, record.payload)?;
        let world = self.world();
        own_space_clip_extent(
            &record.header,
            record.payload,
            bounding_rect.to_float(),
            world.draw_matrix_if_known(record.context.spatial)?,
            |space| world.draw_matrix_if_known(space),
        )
    }
}

struct DiffSide<'a> {
    frame: DisplayListFrame<'a>,
    geometry: SideGeometry<'a>,
    side: FrameSide,
}

impl<'a> DiffSide<'a> {
    fn new(frame: DisplayListFrame<'a>, side: FrameSide) -> Self {
        Self {
            frame,
            geometry: SideGeometry::new(frame.visual_context_tree, frame.scroll_offsets, frame.effect_clip_plan),
            side,
        }
    }

    fn record_at(&self, record: &TopLevelRecordReachingOutsideItsChunk) -> RecordOnTape<'a> {
        RecordOnTape::at(
            self.frame.command_bytes,
            record.record_offset,
            self.frame.command_runs[record.run_index as usize].context,
        )
    }
}

// The viewport bounds that the clips outside every filter on a context's effect chain put on the
// filtered output, or None when no rect clip bounds it. Replay can widen an effect's output clip
// for escaping descendants, so the complete display list's layer plan says which clips are outside.
fn filter_output_clip_bounds(context: ContextRef, side: &SideGeometry<'_>) -> Option<FloatRect> {
    let (tree, effect_clips) = (side.tree, side.effect_clips);
    let mut clip = context.clip;
    let mut effect = context.effect;
    while !effect.is_none() {
        let node = &tree.effect_nodes[effect.0 as usize];
        if let EffectNodeData::Effects(effects) = &node.data
            && (effects.filter.is_some() || effects.backdrop_filter.is_some())
        {
            clip = effect_clips.output_clip(effect);
        }
        effect = node.parent;
    }
    let mut output_bounds: Option<FloatRect> = None;
    while !clip.is_none() {
        let node = &tree.clip_nodes[clip.0 as usize];
        if let ClipNodeData::Rect(data) = &node.data
            && data.mode == ClipMode::Intersect
        {
            let bounds = side
                .world()
                .map_rect_to_root(node.spatial, data.rect)
                .inflated(1.0, 1.0);
            if bounds.x.is_finite() && bounds.y.is_finite() && bounds.width.is_finite() && bounds.height.is_finite() {
                output_bounds = Some(output_bounds.map_or(bounds, |output_bounds| output_bounds.intersected(bounds)));
            }
        }
        clip = node.parent;
    }
    output_bounds
}

fn intersect_like_gfx_int_rect(rect: IntRect, other: IntRect) -> IntRect {
    let left = rect.x.max(other.x);
    let right = rect.right().min(other.right());
    let top = rect.y.max(other.y);
    let bottom = rect.bottom().min(other.bottom());
    if left > right || top > bottom {
        return IntRect::default();
    }
    IntRect::new(left, top, right - left, bottom - top)
}

/// The layer plans of the old and the new frame, which have the same layers. A layer keeps its raster between the
/// frames, so damage is attributed to the layers that draw the content it comes from.
#[derive(Clone, Copy)]
pub struct LayerPlans<'a> {
    pub old: &'a LayerPlan,
    pub new: &'a LayerPlan,
}

#[derive(Clone, Copy)]
enum FrameSide {
    Old,
    New,
}

#[derive(Clone, Copy)]
enum DamagedLayers {
    OfRecord(FrameSide, u32),
    OfRun(FrameSide, DisplayListCommandRun),
    All,
}

#[derive(Clone, Copy, Default)]
struct LayerDamage {
    rect: Option<IntRect>,
    is_unbounded: bool,
}

struct DamageAccumulator<'a> {
    damage_rect: Option<IntRect>,
    changed_unbounded_command: bool,
    viewport_rect: IntRect,
    layer_plans: Option<LayerPlans<'a>>,
    damage_of_layers: Vec<LayerDamage>,
}

impl DamageAccumulator<'_> {
    // Once the damage covers the viewport in every raster it reaches, no further command can change the outcome.
    fn covers_viewport(&self) -> bool {
        let covers = |is_unbounded: bool, rect: Option<IntRect>| {
            is_unbounded || rect.is_some_and(|rect| rect.contains_rect(self.viewport_rect))
        };
        covers(self.changed_unbounded_command, self.damage_rect)
            && self.layer_plans.is_none_or(|plans| {
                plans
                    .new
                    .layers
                    .iter()
                    .zip(&self.damage_of_layers)
                    .all(|(layer, damage)| {
                        matches!(layer.kind, LayerKind::Media { .. }) || covers(damage.is_unbounded, damage.rect)
                    })
            })
    }

    fn for_each_damaged_layer(&mut self, layers: DamagedLayers, mut f: impl FnMut(&mut LayerDamage)) {
        let Some(plans) = self.layer_plans else {
            return;
        };
        let plan_of = |side: FrameSide| match side {
            FrameSide::Old => plans.old,
            FrameSide::New => plans.new,
        };
        match layers {
            DamagedLayers::OfRecord(side, record_offset) => {
                f(&mut self.damage_of_layers[plan_of(side).layer_of_record(record_offset)]);
            }
            DamagedLayers::OfRun(side, run) => {
                for layer in plan_of(side).layers_drawing_records_of(&run) {
                    f(&mut self.damage_of_layers[layer]);
                }
            }
            DamagedLayers::All => self.damage_of_layers.iter_mut().for_each(f),
        }
    }

    fn add_unbounded_damage(&mut self, layers: DamagedLayers) {
        self.changed_unbounded_command = true;
        self.for_each_damaged_layer(layers, |damage| damage.is_unbounded = true);
    }

    fn add_damage_rect(&mut self, rect: IntRect, layers: DamagedLayers) {
        let united =
            |damage_rect: Option<IntRect>| Some(damage_rect.map_or(rect, |damage_rect| damage_rect.united(rect)));
        self.damage_rect = united(self.damage_rect);
        self.for_each_damaged_layer(layers, |damage| damage.rect = united(damage.rect));
    }

    fn add_record_damage(&mut self, side: &DiffSide<'_>, record: &RecordOnTape<'_>) {
        if side.geometry.culling().context_culls_everything(record.context) {
            return;
        }
        if record.header.command_type.is_compositor_metadata() {
            return;
        }
        // The bounds of a command leave out its inline clips in spaces of their own, so they limit it here.
        let rect = match side.geometry.own_space_clip_extent_of(record) {
            Some(OwnSpaceClipExtent::Within(rect)) => rect,
            _ => bounding_rect_computed_from_payload(&record.header, record.payload)
                .unwrap_or_default()
                .to_float(),
        };
        if rect.is_empty() {
            return;
        }
        self.add_changed_content_damage(
            rect,
            record.context,
            &side.geometry,
            DamagedLayers::OfRecord(side.side, record.record_offset),
        );
    }

    fn add_run_damage(&mut self, side: &DiffSide<'_>, run: &DisplayListCommandRun) {
        if side.geometry.culling().context_culls_everything(run.context) {
            return;
        }
        let rect = run.ink_bounds.to_float();
        if rect.is_empty() {
            return;
        }
        self.add_changed_content_damage(rect, run.context, &side.geometry, DamagedLayers::OfRun(side.side, *run));
    }

    // Damage for content drawn within `rect` in `context` that changed, wherever the filters above
    // it spread the change.
    fn add_changed_content_damage(
        &mut self,
        rect: FloatRect,
        context: ContextRef,
        side: &SideGeometry<'_>,
        layers: DamagedLayers,
    ) {
        let nearest_spreading_effects = side.nearest_spreading_effects();
        let spread_by_filters =
            !context.effect.is_none() && !nearest_spreading_effects[context.effect.0 as usize].is_none();
        let (space, extent) = changed_extent_through_filters(
            side.tree,
            nearest_spreading_effects,
            context.spatial,
            rect,
            context.effect,
            side.world(),
        );
        // Spread content can reach far past the clips it was drawn in, but not past those outside
        // the filters.
        let output_clip_bounds = if spread_by_filters {
            filter_output_clip_bounds(context, side)
        } else {
            None
        };
        let mut transformed_rect = match (extent, output_clip_bounds) {
            (Extent::Within(rect), _) => side.world().map_rect_to_root(space, rect),
            (_, Some(output_clip_bounds)) => output_clip_bounds,
            _ => {
                self.add_unbounded_damage(layers);
                return;
            }
        };
        // Transform matrices with entries near float max can overflow the projection to non-finite values.
        // NaN survives both intersect() and is_empty(), so treat such rects as unbounded damage instead of
        // feeding them to enclosing_int_rect(), where the float-to-int conversion would be undefined.
        if !transformed_rect.x.is_finite()
            || !transformed_rect.y.is_finite()
            || !transformed_rect.width.is_finite()
            || !transformed_rect.height.is_finite()
        {
            self.add_unbounded_damage(layers);
            return;
        }
        if let Some(output_clip_bounds) = output_clip_bounds {
            transformed_rect = transformed_rect.intersected(output_clip_bounds);
        }
        // Eye-plane clamping in the projection can produce coordinates beyond integer range, and converting
        // such a float to int is undefined.
        const DAMAGE_COORDINATE_LIMIT: f32 = 16777216.0;
        transformed_rect = transformed_rect.intersected(FloatRect::new(
            -DAMAGE_COORDINATE_LIMIT,
            -DAMAGE_COORDINATE_LIMIT,
            2.0 * DAMAGE_COORDINATE_LIMIT,
            2.0 * DAMAGE_COORDINATE_LIMIT,
        ));
        if transformed_rect.is_empty() {
            return;
        }
        self.add_damage_rect(enclosing_int_rect(transformed_rect), layers);
    }

    // A backdrop filter can read the pixels around its region, so damage beneath the region changes it
    // wherever the filter carries that damage in. The whole region then changes as content of the
    // backdrop's effect: that effect's own filter and the filters above it spread it further, and it
    // can carry the damage on to the backdrop filters above it.
    fn add_backdrop_filter_damage(&mut self, side: &SideGeometry<'_>, command_runs: &[DisplayListCommandRun]) {
        let tree = side.tree;
        let backdrop_of = |effect: EffectNodeIndex| match &tree.effect_nodes[effect.0 as usize].data {
            EffectNodeData::Effects(effects) => effects.backdrop_filter.as_ref(),
            _ => None,
        };
        if self.damage_rect.is_none()
            || !(0..tree.effect_nodes.len()).any(|index| backdrop_of(EffectNodeIndex(index as u32)).is_some())
        {
            return;
        }
        let mut seen_effects = vec![false; tree.effect_nodes.len()];
        // Each effect with a backdrop filter, with the offset of its first record.
        let mut backdrop_effects = Vec::new();
        for run in command_runs {
            let mut effect = run.context.effect;
            while !effect.is_none() && !seen_effects[effect.0 as usize] {
                seen_effects[effect.0 as usize] = true;
                if backdrop_of(effect).is_some() {
                    backdrop_effects.push((effect, run.offset));
                }
                effect = tree.effect_nodes[effect.0 as usize].parent;
            }
        }

        let mut region_is_damaged = vec![false; backdrop_effects.len()];
        loop {
            if self.covers_viewport() {
                return;
            }
            let Some(damage_rect) = self.damage_rect else {
                return;
            };
            let damage = damage_rect.to_float();
            let mut damage_grew = false;
            for (index, &(effect, first_record_offset)) in backdrop_effects.iter().enumerate() {
                if region_is_damaged[index] {
                    continue;
                }
                let node = &tree.effect_nodes[effect.0 as usize];
                let backdrop = backdrop_of(effect).expect("only effects with a backdrop filter are collected");
                let region = backdrop.region.to_float();
                let to_root_matrix = side.world().draw_matrix(node.spatial);
                let reads_damage = (|| {
                    if !to_root_matrix.is_2d_affine() {
                        return None;
                    }
                    let damage_in_effect_space = to_root_matrix.extract_2d_affine().inverse()?.map_rect(damage);
                    let reached = crate::filter_bytes::changed_output_bounds(&backdrop.filter, damage_in_effect_space)?;
                    Some(!reached.intersected(region).is_empty())
                })()
                .unwrap_or(true);
                if !reads_damage {
                    continue;
                }
                region_is_damaged[index] = true;
                damage_grew = true;
                let backdrop_context = ContextRef {
                    spatial: node.spatial,
                    clip: side.effect_clips.output_clip(effect),
                    effect,
                };
                self.add_changed_content_damage(
                    region,
                    backdrop_context,
                    side,
                    DamagedLayers::OfRecord(FrameSide::New, first_record_offset),
                );
            }
            if !damage_grew {
                return;
            }
        }
    }

    fn add_unmatched_chunk_damage(&mut self, side: &DiffSide<'_>, chunk: usize, changed: &ChangedVisualContextNodes) {
        let chunks = side.frame.chunks;
        for run in &side.frame.command_runs[chunks.runs_of(chunk)] {
            if self.covers_viewport() {
                return;
            }
            self.add_run_damage(side, run);
        }
        for record in chunks.records_reaching_outside(chunk) {
            if record.reach == ReachOutsideChunk::ScrollbarPaintedByCompositor
                && changed.chains_changed(side.frame.command_runs[record.run_index as usize].context)
            {
                self.changed_unbounded_command = true;
            }
        }
    }

    // A command that draws in a different space than one of its inline clips shows another part of
    // itself whenever one of the two spaces moves, though neither the command nor its context changed.
    // Where the clips can be mapped into the command's space, command damage covers only what they let it draw.
    fn add_inline_clip_scroll_damage(
        &mut self,
        (old, old_record): (&DiffSide<'_>, &RecordOnTape<'_>),
        (new, new_record): (&DiffSide<'_>, &RecordOnTape<'_>),
        changed: &ChangedVisualContextNodes,
    ) {
        if !inline_clips_of(&new_record.header, new_record.payload)
            .any(|clip| clip.has_own_space && changed.spatial_changed(clip.space))
        {
            return;
        }
        let clips_did_not_move = match (
            old.geometry.own_space_clip_extent_of(old_record),
            new.geometry.own_space_clip_extent_of(new_record),
        ) {
            (None, None) => true,
            (Some(OwnSpaceClipExtent::Within(old_rect)), Some(OwnSpaceClipExtent::Within(new_rect))) => {
                old_rect == new_rect
            }
            _ => false,
        };
        if clips_did_not_move {
            return;
        }
        self.add_record_damage(old, old_record);
        self.add_record_damage(new, new_record);
    }

    fn add_matched_chunk_damage(
        &mut self,
        (old, old_chunk): (&DiffSide<'_>, usize),
        (new, new_chunk): (&DiffSide<'_>, usize),
        changed: &ChangedVisualContextNodes,
        scroll_offsets_differ: bool,
    ) {
        let (old_runs, new_runs) = (old.frame.chunks.runs_of(old_chunk), new.frame.chunks.runs_of(new_chunk));
        debug_assert_eq!(old_runs.len(), new_runs.len(), "chunks of one name hold the same runs");
        let mut records_reaching_outside = old
            .frame
            .chunks
            .records_reaching_outside(old_chunk)
            .iter()
            .zip(new.frame.chunks.records_reaching_outside(new_chunk))
            .peekable();
        for (old_run_index, new_run_index) in old_runs.zip(new_runs) {
            if self.covers_viewport() {
                return;
            }
            let (old_run, new_run) = (
                &old.frame.command_runs[old_run_index],
                &new.frame.command_runs[new_run_index],
            );
            let run_moved = changed.chains_changed(new_run.context);
            let run_changes_layers = self
                .layer_plans
                .is_some_and(|plans| !plans.old.places_run_like(old_run, plans.new, new_run));
            if run_moved || run_changes_layers {
                self.add_run_damage(old, old_run);
                self.add_run_damage(new, new_run);
            }
            while let Some((old_record, new_record)) =
                records_reaching_outside.next_if(|(old_record, _)| old_record.run_index as usize == old_run_index)
            {
                match new_record.reach {
                    ReachOutsideChunk::ScrollbarPaintedByCompositor => {
                        if run_moved {
                            self.changed_unbounded_command = true;
                        }
                    }
                    _ if run_moved || run_changes_layers => {}
                    ReachOutsideChunk::ScrollbarThumbFollowsScrollOffsetOf(scroll_node) => {
                        if scroll_offsets_differ
                            && device_offset_for_index(old.frame.scroll_offsets, scroll_node)
                                != device_offset_for_index(new.frame.scroll_offsets, scroll_node)
                        {
                            self.add_record_damage(old, &old.record_at(old_record));
                            self.add_record_damage(new, &new.record_at(new_record));
                        }
                    }
                    ReachOutsideChunk::InlineClipInItsOwnSpace => {
                        if changed.any_changed {
                            self.add_inline_clip_scroll_damage(
                                (old, &old.record_at(old_record)),
                                (new, &new.record_at(new_record)),
                                changed,
                            );
                        }
                    }
                    ReachOutsideChunk::DeclaresMaskContentOf(_) => {}
                }
            }
        }
    }
}

// This query is cached until scene geometry or scrolling changes. It deliberately ignores clips when
// bounding rotating content, so animation phases cannot reveal pixels outside the computed bounds.
/// Whether the content the tree's animations move at `sample_time_ns` can reach the viewport. The
/// finished animations keep their final values in place; the running ones must all be opacity or
/// plane rotations, whose extents the bounds query can replace, for the answer to be anything but
/// yes.
pub fn animated_content_may_affect_viewport_at(
    command_bytes: &[u8],
    command_runs: &[DisplayListCommandRun],
    tree: &VisualContextTree,
    scroll_offsets: &[FloatPoint],
    viewport_rect: IntRect,
    sample_time_ns: i64,
) -> FfiAnimatedContentViewportEffect {
    let extents = tree.visual_animation_extents_at(sample_time_ns);
    if !extents.all_running_animations_are_bounded {
        return FfiAnimatedContentViewportEffect {
            may_affect_viewport: true,
            stable_until_scene_changes: false,
        };
    }
    let bounded = |tree: &VisualContextTree| {
        animated_content_may_affect_viewport(
            command_bytes,
            command_runs,
            tree,
            scroll_offsets,
            &extents.rotation_nodes,
            &extents.opacity_nodes,
            viewport_rect,
        )
    };
    let may_affect_viewport = if extents.has_finished_animation {
        bounded(&tree.with_visual_animation_samples(sample_time_ns))
    } else {
        bounded(tree)
    };
    FfiAnimatedContentViewportEffect {
        may_affect_viewport,
        stable_until_scene_changes: !extents.has_unfinished_finite_animation,
    }
}

pub fn animated_content_may_affect_viewport(
    command_bytes: &[u8],
    command_runs: &[DisplayListCommandRun],
    tree: &VisualContextTree,
    scroll_offsets: &[FloatPoint],
    rotation_nodes: &[SpatialNodeIndex],
    opacity_nodes: &[EffectNodeIndex],
    viewport_rect: IntRect,
) -> bool {
    let in_rotating_subtree = tree.spatial_nodes_in_subtrees_of(rotation_nodes);
    let mut rotating_nodes = vec![false; tree.spatial_nodes.len()];
    for node in rotation_nodes {
        let Some(flag) = rotating_nodes.get_mut(node.0 as usize) else {
            return true;
        };
        *flag = true;
    }
    let world = OnceCell::new();
    let mut may_affect_viewport = false;
    for run in command_runs {
        let run_bytes = &command_bytes[run.offset as usize..(run.offset + run.size) as usize];
        for_each_command(run_bytes, |header, _, payload| {
            if may_affect_viewport || header.command_type.is_compositor_metadata() {
                return;
            }
            let spatial_is_animated = in_rotating_subtree[run.context.spatial.0 as usize];
            let has_inline_clip_in_its_own_space =
                header.inline_clip_count != 0 && has_inline_clip_in_its_own_space(header, payload);
            if has_inline_clip_in_its_own_space
                && !spatial_is_animated
                && inline_clips_of(header, payload).any(|inline_clip| {
                    inline_clip.has_own_space
                        && in_rotating_subtree
                            .get(inline_clip.space.0 as usize)
                            .is_none_or(|is_animated| *is_animated)
                })
            {
                may_affect_viewport = true;
                return;
            }
            let mut clip = run.context.clip;
            while !clip.is_none() {
                let node = &tree.clip_nodes[clip.0 as usize];
                if !spatial_is_animated && in_rotating_subtree[node.spatial.0 as usize] {
                    may_affect_viewport = true;
                    return;
                }
                clip = node.parent;
            }
            let mut effect = run.context.effect;
            let mut opacity_is_animated = false;
            let mut has_filter = false;
            while !effect.is_none() {
                let node = &tree.effect_nodes[effect.0 as usize];
                opacity_is_animated |= opacity_nodes.contains(&effect);
                if !spatial_is_animated && in_rotating_subtree[node.spatial.0 as usize] {
                    may_affect_viewport = true;
                    return;
                }
                if let EffectNodeData::Effects(effects) = &node.data
                    && (effects.filter.is_some() || effects.backdrop_filter.is_some())
                {
                    has_filter = true;
                }
                effect = node.parent;
            }
            if !spatial_is_animated && !opacity_is_animated {
                return;
            }
            // Filters can expand the output beyond the command bounds.
            if has_filter {
                may_affect_viewport = true;
                return;
            }
            let bounding_rect = bounding_rect_computed_from_payload(header, payload);
            let mut rect = bounding_rect.unwrap_or_default().to_float();
            if has_inline_clip_in_its_own_space && bounding_rect.is_some() {
                let world =
                    world.get_or_init(|| WorldTransforms::new(tree, scroll_offsets, Some(FloatMatrix4x4::identity())));
                if let Some(command_to_root) = world.draw_matrix_if_known(run.context.spatial)
                    && let Some(OwnSpaceClipExtent::Within(clipped_rect)) =
                        own_space_clip_extent(header, payload, rect, command_to_root, |space| {
                            world.draw_matrix_if_known(space)
                        })
                {
                    if clipped_rect.is_empty() {
                        return;
                    }
                    rect = clipped_rect;
                }
            }
            let Some(bounds) =
                tree.rect_with_rotation_bounds_to_viewport(run.context.spatial, rect, &rotating_nodes, scroll_offsets)
            else {
                may_affect_viewport = true;
                return;
            };
            let bounds = bounds.inflated(2.0, 2.0);
            may_affect_viewport = bounds.x <= viewport_rect.right() as f32
                && bounds.right() >= viewport_rect.x as f32
                && bounds.y <= viewport_rect.bottom() as f32
                && bounds.bottom() >= viewport_rect.y as f32;
        });
    }
    may_affect_viewport
}

// One frame's display list with the tree and scroll offsets it was replayed under.
#[derive(Clone, Copy)]
pub struct DisplayListFrame<'a> {
    pub command_bytes: &'a [u8],
    pub command_runs: &'a [DisplayListCommandRun],
    pub chunks: &'a TapeChunks,
    pub effect_clip_plan: &'a EffectClipPlan,
    pub summary: &'a DisplayListSummary,
    pub visual_context_tree: &'a VisualContextTree,
    pub scroll_offsets: &'a [FloatPoint],
}

// The canvases, video sinks, image frames and frames of nested contexts whose pixels changed since the old frame.
// The lists draw them with commands that stay the same, so the diff cannot see the change.
#[derive(Clone, Copy, Default)]
pub struct LiveContentChanges<'a> {
    pub canvases: &'a [CanvasId],
    pub video_sinks: &'a [VideoSinkHandle],
    pub image_frames: &'a [ImageFrameResourceId],
    pub composited_contexts: &'a [CompositorContextId],
    /// What the compositor repaints whatever the lists hold, such as a blinking caret.
    pub forced_damage_rect: IntRect,
    pub offsets_of_records_composited_as_media_layers: &'a [u32],
}

#[derive(Clone, Debug, PartialEq)]
pub struct DisplayListDamage {
    pub rect: Option<IntRect>,
    /// With the frames' layer plans, the damage of each layer, none where that has no bounds.
    pub layer_rects: Vec<Option<IntRect>>,
}

impl DisplayListDamage {
    pub fn unbounded(layer_count: usize) -> Self {
        Self {
            rect: None,
            layer_rects: vec![None; layer_count],
        }
    }
}

impl LiveContentChanges<'_> {
    fn is_empty(&self) -> bool {
        self.canvases.is_empty()
            && self.video_sinks.is_empty()
            && self.image_frames.is_empty()
            && self.composited_contexts.is_empty()
    }

    fn includes(&self, content: LiveContent) -> bool {
        match content {
            LiveContent::Canvas(canvas_id) => self.canvases.contains(&canvas_id),
            LiveContent::VideoFrame(video_sink_handle) => self.video_sinks.contains(&video_sink_handle),
            LiveContent::ImageFrame(image_frame_id) => self.image_frames.contains(&image_frame_id),
            LiveContent::CompositedContext(context_id) => self.composited_contexts.contains(&context_id),
        }
    }
}

// The viewport-space damage between two frames, or None when a change without bounds needs a
// full repaint. The diff pairs the chunks of the two lists by name in paint order: a chunk without
// a partner is damaged run by run, and a paired chunk only where its runs' visual contexts, its
// records' layers or what its records read outside it changed. The diff stops as soon as the
// damage covers the viewport, so a later unbounded change may then be reported as the viewport
// rect instead of None. Given the frames' layer plans, the damage of each layer is reported too.
pub fn compute_display_list_damage(
    old_frame: DisplayListFrame<'_>,
    new_frame: DisplayListFrame<'_>,
    viewport_rect: IntRect,
    live_content_changes: LiveContentChanges<'_>,
    layer_plans: Option<LayerPlans<'_>>,
) -> DisplayListDamage {
    let layer_count = layer_plans.map_or(0, |plans| plans.new.layers.len());
    let old = DiffSide::new(old_frame, FrameSide::Old);
    let new = DiffSide::new(new_frame, FrameSide::New);
    let mut damage = DamageAccumulator {
        damage_rect: None,
        changed_unbounded_command: false,
        viewport_rect,
        layer_plans,
        damage_of_layers: vec![LayerDamage::default(); layer_count],
    };

    let same_chunk_list = std::ptr::eq(old_frame.chunks, new_frame.chunks);
    let paired_by_name = if same_chunk_list {
        Vec::new()
    } else {
        pair_chunks_by_name(old_frame.chunks, new_frame.chunks)
    };
    let pairings = || {
        (0..if same_chunk_list { new_frame.chunks.len() } else { 0 })
            .map(|index| ChunkPairing::Matched { old: index, new: index })
            .chain(paired_by_name.iter().copied())
    };
    let scroll_offsets_differ = old_frame.scroll_offsets != new_frame.scroll_offsets;
    let changed = ChangedVisualContextNodes::between(&old, &new, scroll_offsets_differ, pairings());
    let nothing_in_paired_chunks_can_differ = same_chunk_list
        && !changed.any_changed
        && !scroll_offsets_differ
        && layer_plans.is_none_or(|plans| plans.old.places_records_like(plans.new));
    if !nothing_in_paired_chunks_can_differ {
        for pairing in pairings() {
            if damage.covers_viewport() {
                break;
            }
            match pairing {
                ChunkPairing::Matched {
                    old: old_chunk,
                    new: new_chunk,
                } => damage.add_matched_chunk_damage(
                    (&old, old_chunk),
                    (&new, new_chunk),
                    &changed,
                    scroll_offsets_differ,
                ),
                ChunkPairing::OnlyOld(chunk) => damage.add_unmatched_chunk_damage(&old, chunk, &changed),
                ChunkPairing::OnlyNew(chunk) => damage.add_unmatched_chunk_damage(&new, chunk, &changed),
            }
        }
    }

    // A record that draws changed live content inside a group is damaged as the whole group.
    if !live_content_changes.is_empty() {
        for live_content_record in &new_frame.summary.live_content_records {
            if damage.covers_viewport() {
                break;
            }
            if !live_content_changes.includes(live_content_record.content)
                || live_content_changes
                    .offsets_of_records_composited_as_media_layers
                    .contains(&live_content_record.record.record_offset)
            {
                continue;
            }
            damage.add_record_damage(
                &new,
                &RecordOnTape::at(
                    new_frame.command_bytes,
                    live_content_record.record.record_offset,
                    new_frame.command_runs[live_content_record.record.run_index as usize].context,
                ),
            );
        }
    }

    // NB: A backdrop filter reads more than the part of its region that is repainted, so forced damage goes through it too.
    let forced_damage_rect = live_content_changes.forced_damage_rect;
    if !forced_damage_rect.is_empty() {
        damage.add_damage_rect(forced_damage_rect, DamagedLayers::All);
    }
    damage.add_backdrop_filter_damage(&new.geometry, new_frame.command_runs);

    let bounded_damage_rect = |is_unbounded: bool, damage_rect: Option<IntRect>| {
        if is_unbounded {
            return None;
        }
        Some(damage_rect.map_or(IntRect::default(), |damage_rect| {
            intersect_like_gfx_int_rect(damage_rect.inflated_edges(1, 1, 1, 1), viewport_rect)
        }))
    };
    DisplayListDamage {
        rect: bounded_damage_rect(damage.changed_unbounded_command, damage.damage_rect),
        layer_rects: damage
            .damage_of_layers
            .iter()
            .map(|layer_damage| bounded_damage_rect(layer_damage.is_unbounded, layer_damage.rect))
            .collect(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::display_list::builder::note_command;
    use crate::display_list::chunks::{DisplayListChunk, chunk_id};
    use crate::display_list::commands::{
        BackdropFilterRegion, CanvasId, CompositorMainThreadWheelEventRegion, CompositorScrollbar, DeclareMaskContent,
        DisplayListCommand, DisplayListDataSpan, DisplayListInlineClip, DrawCanvas, DrawCompositedContext,
        DrawGlyphRun, DrawIsolatedGroup, DrawScaledDecodedImageFrame, DrawVideoFrame, FillRect, FontResourceId,
        INLINE_CLIP_ENTRY_SIZE, InlineClipKind, OptionalColor, OptionalFloatRect, PaintScrollBar,
    };
    use crate::display_list::ffi_bytes::FfiBytes;
    use crate::display_list::summary::summarize;
    use crate::node_slot_id::NodeSlotId;
    use crate::visual_context::scroll_state::NO_SCROLL_STATE_SLOT;
    use crate::visual_context::{
        BackdropFilterData, ClipData, ClipMode, ClipNodeData, ClipNodeIndex, ClipPathData, EffectNodeData,
        EffectNodeIndex, EffectsData, MaskData, MaskLayerOrigin, ScrollData, SpatialData, TransformData,
        TransformDataRole,
    };
    use libgfx_rust::filter::Filter;
    use libgfx_rust::path::Path;
    use libgfx_rust::{
        Color, ColorFilterType, CompositingAndBlendingOperator, CornerRadii, FloatMatrix4x4, FloatPoint, MaskKind,
        Orientation, ScalingMode, WindingRule, translation_matrix,
    };

    const RED: Color = Color(0xffff0000);
    const GREEN: Color = Color(0xff00ff00);
    const BLUE: Color = Color(0xff0000ff);
    const YELLOW: Color = Color(0xffffff00);
    const CYAN: Color = Color(0xff00ffff);

    #[derive(Clone, Default)]
    struct TestDisplayList {
        bytes: Vec<u8>,
        runs: Vec<DisplayListCommandRun>,
        chunks: Vec<DisplayListChunk>,
    }

    impl std::ops::Deref for TestDisplayList {
        type Target = [u8];

        fn deref(&self) -> &Self::Target {
            &self.bytes
        }
    }

    impl TestDisplayList {
        fn first_run_of_open_chunk(&self) -> usize {
            self.chunks.last().map_or(0, |chunk| chunk.run_end as usize)
        }

        fn end_chunk(&mut self) {
            let first_run = self.first_run_of_open_chunk();
            if first_run == self.runs.len() {
                return;
            }
            let chunk_start = self.chunks.last().map_or(0, |chunk| chunk.byte_end);
            self.chunks.push(DisplayListChunk {
                id: chunk_id(
                    &self.bytes[chunk_start as usize..],
                    &self.runs[first_run..],
                    chunk_start,
                ),
                byte_end: self.bytes.len() as u32,
                run_end: self.runs.len() as u32,
            });
        }

        fn extend_from_slice(&mut self, source: &Self) {
            let mut source = source.clone();
            source.end_chunk();
            self.end_chunk();
            let (byte_base, run_base) = (self.bytes.len() as u32, self.runs.len() as u32);
            self.bytes.extend_from_slice(&source.bytes);
            self.runs.extend(source.runs.iter().map(|run| DisplayListCommandRun {
                offset: run.offset + byte_base,
                ..*run
            }));
            self.chunks.extend(source.chunks.iter().map(|chunk| DisplayListChunk {
                byte_end: chunk.byte_end + byte_base,
                run_end: chunk.run_end + run_base,
                ..*chunk
            }));
        }

        fn prepared_against(&self, tree: &VisualContextTree) -> PreparedList<'_> {
            let mut list = self.clone();
            list.end_chunk();
            PreparedList {
                list: self,
                chunks: TapeChunks::of_tape(&list.bytes, &list.runs, &list.chunks),
                effect_clip_plan: EffectClipPlan::new(tree, &self.runs).unwrap(),
                summary: summarize(&self.bytes, &self.runs),
            }
        }
    }

    struct PreparedList<'a> {
        list: &'a TestDisplayList,
        chunks: TapeChunks,
        effect_clip_plan: EffectClipPlan,
        summary: DisplayListSummary,
    }

    impl<'a> PreparedList<'a> {
        fn frame(&'a self, tree: &'a VisualContextTree, scroll_offsets: &'a [FloatPoint]) -> DisplayListFrame<'a> {
            DisplayListFrame {
                command_bytes: &self.list.bytes,
                command_runs: &self.list.runs,
                chunks: &self.chunks,
                effect_clip_plan: &self.effect_clip_plan,
                summary: &self.summary,
                visual_context_tree: tree,
                scroll_offsets,
            }
        }
    }

    // Appends one record the way the Rust builder writes it: header, payload, then zero padding up to
    // the command alignment, but with the inline clip entries chosen by the test.
    fn append_record<C: DisplayListCommand>(
        display_list: &mut TestDisplayList,
        command: &C,
        inline_data: &[u8],
        context: ContextRef,
        inline_clips: &[DisplayListInlineClip],
    ) {
        let bytes = &mut display_list.bytes;
        let unpadded_payload_size = std::mem::size_of::<C>() + inline_data.len();
        let entries_size = inline_clips.len() * INLINE_CLIP_ENTRY_SIZE;
        let padded_record_size = (HEADER_SIZE + unpadded_payload_size + entries_size).next_multiple_of(16);
        let header = DisplayListCommandHeader {
            command_type: C::COMMAND_TYPE,
            inline_clip_count: inline_clips.len() as u8,
            has_inline_transform: false,
            payload_size: (padded_record_size - HEADER_SIZE) as u32,
        };
        let start = bytes.len();
        bytes.resize(start + padded_record_size, 0);
        header.write_ffi_bytes(&mut bytes[start..start + HEADER_SIZE]);
        let payload_start = start + HEADER_SIZE;
        command.write_ffi_bytes(&mut bytes[payload_start..payload_start + std::mem::size_of::<C>()]);
        let inline_start = payload_start + std::mem::size_of::<C>();
        bytes[inline_start..inline_start + inline_data.len()].copy_from_slice(inline_data);
        let entries_start = start + padded_record_size - entries_size;
        for (index, entry) in inline_clips.iter().enumerate() {
            let entry_start = entries_start + index * INLINE_CLIP_ENTRY_SIZE;
            entry.write_ffi_bytes(&mut bytes[entry_start..entry_start + INLINE_CLIP_ENTRY_SIZE]);
        }
        let bounding_rect =
            bounding_rect_computed_from_payload(&header, &bytes[payload_start..start + padded_record_size]);
        if display_list.runs.len() == display_list.first_run_of_open_chunk() {
            display_list.runs.push(DisplayListCommandRun {
                offset: start as u32,
                context,
                ..DisplayListCommandRun::default()
            });
        }
        note_command(
            &mut display_list.runs,
            C::COMMAND_TYPE,
            bounding_rect,
            context,
            start,
            padded_record_size,
        );
    }

    fn command_bytes<C: DisplayListCommand>(command: &C, context: ContextRef) -> TestDisplayList {
        let mut bytes = TestDisplayList::default();
        append_record(&mut bytes, command, &[], context, &[]);
        bytes.end_chunk();
        bytes
    }

    fn list_of(parts: &[&TestDisplayList]) -> TestDisplayList {
        let mut list = TestDisplayList::default();
        for part in parts {
            list.extend_from_slice(part);
        }
        list
    }

    fn fill_command_bytes(rect: IntRect, color: Color) -> TestDisplayList {
        command_bytes(
            &FillRect {
                rect,
                color,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: EffectNodeIndex::NONE,
            },
            ContextRef::default(),
        )
    }

    fn rect_inline_clip(rect: IntRect) -> DisplayListInlineClip {
        DisplayListInlineClip {
            clip_rect_or_path_device_bounds: rect.to_float(),
            corner_radii: CornerRadii::default(),
            path_data: DisplayListDataSpan::default(),
            path_winding_rule: WindingRule::Nonzero,
            kind: InlineClipKind::Rect,
            mode: ClipMode::Intersect,
            has_own_space: false,
            space: SpatialNodeIndex::default(),
            padding: 0,
        }
    }

    fn canvas_command_bytes(rect: IntRect) -> TestDisplayList {
        let command = DrawCanvas {
            dst_rect: rect,
            canvas_id: CanvasId(1),
            scaling_mode: ScalingMode::NearestNeighbor,
        };
        command_bytes(&command, ContextRef::default())
    }

    fn transform(matrix: FloatMatrix4x4) -> TransformData {
        TransformData {
            matrix,
            origin: FloatPoint::default(),
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
            bounds: FloatRect::default(),
        }
    }

    fn identity_tree() -> VisualContextTree {
        VisualContextTree::create(transform(FloatMatrix4x4::identity()))
    }

    fn clip(rect: FloatRect) -> ClipNodeData {
        ClipNodeData::Rect(ClipData {
            rect,
            corner_radii: CornerRadii::default(),
            mode: ClipMode::Intersect,
        })
    }

    fn mask(rect: IntRect) -> EffectNodeData {
        EffectNodeData::Mask(MaskData {
            rect,
            kind: MaskKind::Alpha,
            origin: MaskLayerOrigin::CssMaskLayers,
            masks_backdrop: false,
        })
    }

    fn effects(filter: Vec<u8>) -> EffectNodeData {
        EffectNodeData::Effects(EffectsData {
            isolation: false,
            opacity: 1.0,
            blend_mode: CompositingAndBlendingOperator::Normal,
            filter: Some(std::sync::Arc::new(filter)),
            backdrop_filter: None,
        })
    }

    // A context under a fresh root clip, or under a fresh root effect without an output clip.
    fn clip_context(tree: &mut VisualContextTree, data: ClipNodeData) -> ContextRef {
        let clip = tree.append_clip(data, ClipNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX);
        ContextRef {
            clip,
            effect: EffectNodeIndex::NONE,
            ..ContextRef::default()
        }
    }

    fn effect_context(tree: &mut VisualContextTree, data: EffectNodeData) -> ContextRef {
        let effect = tree.append_effect(
            data,
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );
        ContextRef {
            clip: ClipNodeIndex::NONE,
            effect,
            ..ContextRef::default()
        }
    }

    fn blur_filter(radius: f32) -> Vec<u8> {
        Filter::blur(radius, radius, None).serialize()
    }

    fn color_filter(amount: f32) -> Vec<u8> {
        Filter::color(ColorFilterType::Brightness, amount, None).serialize()
    }

    fn drop_shadow_filter(offset: f32, radius: f32) -> Vec<u8> {
        Filter::drop_shadow(offset, offset, radius, RED, None).serialize()
    }

    fn damage_for_filter_change(old_filter: Vec<u8>, new_filter: Vec<u8>) -> Option<IntRect> {
        let mut old_tree = identity_tree();
        let old_context = effect_context(&mut old_tree, effects(old_filter));
        let mut new_tree = identity_tree();
        let new_context = effect_context(&mut new_tree, effects(new_filter));
        let rect = IntRect::new(10, 10, 20, 20);
        let fill = FillRect {
            rect,
            color: RED,
            compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            background_color_animation_effect: EffectNodeIndex::NONE,
        };
        let old_display_list = command_bytes(&fill, context_in(VISUAL_VIEWPORT_NODE_INDEX, old_context));
        let new_display_list = command_bytes(&fill, context_in(VISUAL_VIEWPORT_NODE_INDEX, new_context));
        damage(&old_display_list, &old_tree, &new_display_list, &new_tree)
    }

    fn damage(
        old_bytes: &TestDisplayList,
        old_tree: &VisualContextTree,
        new_bytes: &TestDisplayList,
        new_tree: &VisualContextTree,
    ) -> Option<IntRect> {
        damage_with_scroll_offsets(
            old_bytes,
            old_tree,
            &[],
            new_bytes,
            new_tree,
            &[],
            IntRect::new(0, 0, 100, 100),
        )
    }

    fn damage_with_scroll_offsets(
        old_bytes: &TestDisplayList,
        old_tree: &VisualContextTree,
        old_scroll_offsets: &[FloatPoint],
        new_bytes: &TestDisplayList,
        new_tree: &VisualContextTree,
        new_scroll_offsets: &[FloatPoint],
        viewport_rect: IntRect,
    ) -> Option<IntRect> {
        let (old_list, new_list) = (
            old_bytes.prepared_against(old_tree),
            new_bytes.prepared_against(new_tree),
        );
        compute_display_list_damage(
            old_list.frame(old_tree, old_scroll_offsets),
            new_list.frame(new_tree, new_scroll_offsets),
            viewport_rect,
            LiveContentChanges::default(),
            None,
        )
        .rect
    }

    fn context_in(spatial: SpatialNodeIndex, context: ContextRef) -> ContextRef {
        ContextRef { spatial, ..context }
    }

    fn damage_for_live_content_changes(
        bytes: &TestDisplayList,
        tree: &VisualContextTree,
        live_content_changes: LiveContentChanges<'_>,
    ) -> Option<IntRect> {
        let list = bytes.prepared_against(tree);
        let frame = list.frame(tree, &[]);
        compute_display_list_damage(frame, frame, IntRect::new(0, 0, 100, 100), live_content_changes, None).rect
    }

    #[test]
    fn a_record_composited_as_a_media_layer_is_not_damaged_by_its_live_content() {
        let tree = identity_tree();
        let mut list = canvas_command_bytes(IntRect::new(10, 10, 20, 20));
        let media_layer_record_offset = 0;
        list.extend_from_slice(&fill_command_bytes(IntRect::new(50, 50, 10, 10), RED));
        list.extend_from_slice(&canvas_command_bytes(IntRect::new(60, 60, 10, 10)));
        let changed_canvas = LiveContentChanges {
            canvases: &[CanvasId(1)],
            offsets_of_records_composited_as_media_layers: &[media_layer_record_offset],
            ..LiveContentChanges::default()
        };
        assert_eq!(
            damage_for_live_content_changes(&list, &tree, changed_canvas),
            Some(IntRect::new(59, 59, 12, 12))
        );
    }

    #[test]
    fn changed_live_content_damages_where_it_is_drawn() {
        let tree = identity_tree();
        let rect = IntRect::new(10, 10, 20, 20);
        let canvas = canvas_command_bytes(rect);
        let changed_canvas = LiveContentChanges {
            canvases: &[CanvasId(1)],
            ..LiveContentChanges::default()
        };
        assert_eq!(
            damage_for_live_content_changes(&canvas, &tree, changed_canvas),
            Some(IntRect::new(9, 9, 22, 22))
        );
        let other_canvas = LiveContentChanges {
            canvases: &[CanvasId(2)],
            ..LiveContentChanges::default()
        };
        assert_eq!(
            damage_for_live_content_changes(&canvas, &tree, other_canvas),
            Some(IntRect::default())
        );

        let video = command_bytes(
            &DrawVideoFrame {
                dst_rect: rect,
                video_sink_handle: VideoSinkHandle(1),
                scaling_mode: ScalingMode::NearestNeighbor,
            },
            ContextRef::default(),
        );
        let changed_video = LiveContentChanges {
            video_sinks: &[VideoSinkHandle(1)],
            ..LiveContentChanges::default()
        };
        assert_eq!(
            damage_for_live_content_changes(&video, &tree, changed_video),
            Some(IntRect::new(9, 9, 22, 22))
        );
        assert_eq!(
            damage_for_live_content_changes(&video, &tree, changed_canvas),
            Some(IntRect::default())
        );

        let image = command_bytes(
            &DrawScaledDecodedImageFrame {
                dst_rect: FloatRect::new(10.0, 10.0, 20.0, 20.0),
                src_rect: OptionalFloatRect::default(),
                frame_id: ImageFrameResourceId(1),
                scaling_mode: ScalingMode::NearestNeighbor,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                isolated_backdrop_color: OptionalColor::default(),
                apply_force_dark: false,
            },
            ContextRef::default(),
        );
        let changed_image = LiveContentChanges {
            image_frames: &[ImageFrameResourceId(1)],
            ..LiveContentChanges::default()
        };
        assert_eq!(
            damage_for_live_content_changes(&image, &tree, changed_image),
            Some(IntRect::new(9, 9, 22, 22))
        );
        assert_eq!(
            damage_for_live_content_changes(&image, &tree, changed_video),
            Some(IntRect::default())
        );

        let nested_context = command_bytes(
            &DrawCompositedContext {
                dst_rect: FloatRect::new(10.0, 10.0, 20.0, 20.0),
                child_context_id: CompositorContextId(7),
                scaling_mode: ScalingMode::NearestNeighbor,
                spatial_node_index: VISUAL_VIEWPORT_NODE_INDEX,
                snap_origin_if_untransformed: true,
            },
            ContextRef::default(),
        );
        let changed_nested_context = LiveContentChanges {
            composited_contexts: &[CompositorContextId(7)],
            ..LiveContentChanges::default()
        };
        assert_eq!(
            damage_for_live_content_changes(&nested_context, &tree, changed_nested_context),
            Some(IntRect::new(9, 9, 22, 22))
        );
        let other_nested_context = LiveContentChanges {
            composited_contexts: &[CompositorContextId(8)],
            ..LiveContentChanges::default()
        };
        assert_eq!(
            damage_for_live_content_changes(&nested_context, &tree, other_nested_context),
            Some(IntRect::default())
        );
    }

    #[test]
    fn changed_live_content_under_a_blur_damages_its_blurred_footprint() {
        let mut tree = identity_tree();
        let context = context_in(
            VISUAL_VIEWPORT_NODE_INDEX,
            effect_context(&mut tree, effects(blur_filter(2.0))),
        );
        let rect = IntRect::new(10, 10, 20, 20);
        let canvas = command_bytes(
            &DrawCanvas {
                dst_rect: rect,
                canvas_id: CanvasId(1),
                scaling_mode: ScalingMode::NearestNeighbor,
            },
            context,
        );
        let changed_canvas = LiveContentChanges {
            canvases: &[CanvasId(1)],
            ..LiveContentChanges::default()
        };
        assert_eq!(
            damage_for_live_content_changes(&canvas, &tree, changed_canvas),
            Some(IntRect::new(3, 3, 34, 34))
        );
    }

    #[test]
    fn changed_live_content_inside_a_group_damages_the_whole_group() {
        let tree = identity_tree();
        let group_rect = IntRect::new(5, 5, 40, 40);
        let content = canvas_command_bytes(IntRect::new(10, 10, 20, 20));
        let no_data = DisplayListDataSpan { offset: 0, size: 0 };
        let mut group = TestDisplayList::default();
        append_record(
            &mut group,
            &DrawIsolatedGroup {
                clip_rect: group_rect.to_float(),
                content: DisplayListDataSpan {
                    offset: std::mem::size_of::<DrawIsolatedGroup>() as u32,
                    size: content.len() as u32,
                },
                mask: no_data,
                filter: no_data,
                opacity: 1.0,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                mask_kind: MaskKind::Alpha,
            },
            &content,
            ContextRef::default(),
            &[],
        );
        let changed_canvas = LiveContentChanges {
            canvases: &[CanvasId(1)],
            ..LiveContentChanges::default()
        };
        assert_eq!(
            damage_for_live_content_changes(&group, &tree, changed_canvas),
            Some(IntRect::new(4, 4, 42, 42))
        );
    }

    #[test]
    fn content_under_nested_fractional_scrolls_is_damaged_where_replay_draws_it() {
        let mut tree = identity_tree();
        let mut scroll_node = VISUAL_VIEWPORT_NODE_INDEX;
        for _ in 0..3 {
            scroll_node = tree.append_spatial(
                SpatialData::Scroll(ScrollData {
                    state_slot: NO_SCROLL_STATE_SLOT,
                    owner_paintable: NodeSlotId::INVALID,
                }),
                scroll_node,
            );
        }
        let mut scroll_offsets = vec![FloatPoint { x: 0.0, y: -0.9 }; 4];
        scroll_offsets[0] = FloatPoint::default();
        let rect = IntRect::new(10, 10, 20, 20);
        let new_commands = command_bytes(
            &FillRect {
                rect,
                color: RED,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: EffectNodeIndex::NONE,
            },
            context_in(scroll_node, ContextRef::default()),
        );
        // Each of the three offsets rounds to a whole pixel, so the rect is drawn 3px above where it was recorded.
        assert_eq!(
            damage_with_scroll_offsets(
                &TestDisplayList::default(),
                &tree,
                &scroll_offsets,
                &new_commands,
                &tree,
                &scroll_offsets,
                IntRect::new(0, 0, 100, 100),
            ),
            Some(IntRect::new(9, 6, 22, 22))
        );
    }

    #[test]
    fn identical_display_lists_have_no_damage() {
        let tree = identity_tree();
        let display_list = fill_command_bytes(IntRect::new(10, 10, 20, 20), RED);
        assert_eq!(
            damage(&display_list, &tree, &display_list, &tree),
            Some(IntRect::default())
        );
    }

    #[test]
    fn removing_a_clip_escape_damages_unchanged_runs_whose_layer_placement_changes() {
        let mut tree = identity_tree();
        let local_clip = tree.append_clip(
            clip(FloatRect::new(0.0, 0.0, 30.0, 30.0)),
            ClipNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let effect = tree.append_effect(
            EffectNodeData::Effects(EffectsData {
                isolation: false,
                opacity: 0.5,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
                backdrop_filter: None,
            }),
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            local_clip,
        );
        let rect = IntRect::new(10, 10, 10, 10);
        let fill = FillRect {
            rect,
            color: RED,
            compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            background_color_animation_effect: EffectNodeIndex::NONE,
        };
        let remaining = command_bytes(
            &fill,
            ContextRef {
                clip: local_clip,
                effect,
                ..ContextRef::default()
            },
        );
        let escaped_rect = IntRect::new(70, 70, 10, 10);
        let escaped = command_bytes(
            &FillRect {
                rect: escaped_rect,
                ..fill
            },
            ContextRef {
                clip: ClipNodeIndex::NONE,
                effect,
                ..ContextRef::default()
            },
        );
        let mut before = remaining.clone();
        before.extend_from_slice(&escaped);
        let changed = damage(&before, &tree, &remaining, &tree).unwrap();
        // Repaint both the removed drawing and the retained drawing whose layer moved
        // inside the clip. Comparing only the unchanged AVC tree would miss the latter.
        assert!(changed.x <= rect.x && changed.y <= rect.y);
        assert!(changed.x + changed.width >= escaped_rect.x + escaped_rect.width);
        assert!(changed.y + changed.height >= escaped_rect.y + escaped_rect.height);
    }

    #[test]
    fn changed_scroll_offset_damages_scrollbar() {
        let tree = identity_tree();

        let scrollbar = PaintScrollBar {
            scroll_node_index: SpatialNodeIndex(1),
            gutter_rect: IntRect::default(),
            thumb_rect: IntRect::new(10, 12, 20, 8),
            track_rect: IntRect::new(10, 10, 80, 12),
            expanded_gutter_rect: IntRect::default(),
            scroll_size: 0.75,
            thumb_color: RED,
            thumb_outline_color: Color::TRANSPARENT,
            track_color: Color::TRANSPARENT,
            vertical: false,
        };

        let display_list = command_bytes(&scrollbar, ContextRef::default());

        assert_eq!(
            damage_with_scroll_offsets(
                &display_list,
                &tree,
                &[FloatPoint::default(), FloatPoint::default()],
                &display_list,
                &tree,
                &[FloatPoint::default(), FloatPoint { x: -40.0, y: 0.0 }],
                IntRect::new(0, 0, 100, 100),
            ),
            Some(IntRect::new(9, 9, 82, 14))
        );
    }

    #[test]
    fn changed_scroll_offset_damages_a_draw_with_an_inline_clip_in_another_space() {
        let mut tree = identity_tree();
        let scroll = tree.append_spatial(
            SpatialData::Scroll(ScrollData {
                state_slot: 1,
                owner_paintable: NodeSlotId::INVALID,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let rect = IntRect::new(10, 10, 20, 20);
        let draw_with_clip = |clip: DisplayListInlineClip| {
            let mut display_list = TestDisplayList::default();
            append_record(
                &mut display_list,
                &FillRect {
                    rect,
                    color: RED,
                    compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                    background_color_animation_effect: EffectNodeIndex::NONE,
                },
                &[],
                ContextRef::default(),
                &[clip],
            );
            display_list
        };
        let damage_after_scroll = |display_list: &TestDisplayList| {
            damage_with_scroll_offsets(
                display_list,
                &tree,
                &[FloatPoint::default(), FloatPoint::default()],
                display_list,
                &tree,
                &[FloatPoint::default(), FloatPoint { x: 0.0, y: -40.0 }],
                IntRect::new(0, 0, 100, 100),
            )
        };

        let clip_in_space = |space: SpatialNodeIndex| DisplayListInlineClip {
            has_own_space: true,
            space,
            ..rect_inline_clip(rect)
        };

        assert!(damage_after_scroll(&draw_with_clip(rect_inline_clip(rect))).is_none_or(|damage| damage.is_empty()));
        // A clip whose space does not move with the scroll leaves the draw as it was.
        assert!(
            damage_after_scroll(&draw_with_clip(clip_in_space(VISUAL_VIEWPORT_NODE_INDEX)))
                .is_none_or(|damage| damage.is_empty())
        );
        // The scroll moves the clip off the draw, so only where it was changes.
        assert_eq!(
            damage_after_scroll(&draw_with_clip(clip_in_space(scroll))),
            Some(IntRect::new(9, 9, 22, 22))
        );
        // A partial move changes only the part of the draw that the clip uncovers or covers.
        let damage_after_small_scroll = |display_list: &TestDisplayList| {
            damage_with_scroll_offsets(
                display_list,
                &tree,
                &[FloatPoint::default(), FloatPoint::default()],
                display_list,
                &tree,
                &[FloatPoint::default(), FloatPoint { x: 0.0, y: -5.0 }],
                IntRect::new(0, 0, 100, 100),
            )
        };
        let small_scroll_damage = damage_after_small_scroll(&draw_with_clip(DisplayListInlineClip {
            has_own_space: true,
            space: scroll,
            ..rect_inline_clip(IntRect::new(10, 10, 20, 5))
        }));
        assert!(small_scroll_damage.is_some_and(|damage| damage.height < 22 && !damage.is_empty()));
    }

    #[test]
    fn a_new_tree_that_moves_the_space_of_an_inline_clip_damages_its_draw() {
        let mut old_tree = identity_tree();
        let old_space = old_tree.append_spatial(
            SpatialData::Transform(transform(FloatMatrix4x4::identity())),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        // The new tree gives the index of the clip's space to a node that sits elsewhere.
        let mut new_tree = identity_tree();
        let new_space = new_tree.append_spatial(
            SpatialData::Transform(transform(translation_matrix(0.0, 40.0, 0.0))),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        assert_eq!(old_space, new_space);
        let rect = IntRect::new(10, 10, 20, 20);
        let mut display_list = TestDisplayList::default();
        append_record(
            &mut display_list,
            &FillRect {
                rect,
                color: RED,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: EffectNodeIndex::NONE,
            },
            &[],
            ContextRef::default(),
            &[DisplayListInlineClip {
                has_own_space: true,
                space: old_space,
                ..rect_inline_clip(rect)
            }],
        );
        let offsets = [FloatPoint::default()];
        let viewport = IntRect::new(0, 0, 100, 100);
        assert!(
            damage_with_scroll_offsets(
                &display_list,
                &old_tree,
                &offsets,
                &display_list,
                &old_tree,
                &offsets,
                viewport
            )
            .is_none_or(|damage| damage.is_empty())
        );
        assert_eq!(
            damage_with_scroll_offsets(
                &display_list,
                &old_tree,
                &offsets,
                &display_list,
                &new_tree,
                &offsets,
                viewport
            ),
            Some(IntRect::new(9, 9, 22, 22))
        );
    }

    #[test]
    fn animated_content_follows_inline_clips_in_spaces_of_their_own() {
        let mut tree = identity_tree();
        let scroll = tree.append_spatial(
            SpatialData::Scroll(ScrollData {
                state_slot: 1,
                owner_paintable: NodeSlotId::INVALID,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let rotating = tree.append_spatial(
            SpatialData::Transform(transform(FloatMatrix4x4::identity())),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let effect = tree.append_effect(
            EffectNodeData::Effects(EffectsData {
                isolation: false,
                opacity: 0.5,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
                backdrop_filter: None,
            }),
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );
        let viewport = IntRect::new(0, 0, 800, 600);
        let draw_clipped_to = |clip_space: SpatialNodeIndex, clip_rect: IntRect, context: ContextRef| {
            let mut display_list = TestDisplayList::default();
            append_record(
                &mut display_list,
                &FillRect {
                    rect: viewport,
                    color: RED,
                    compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                    background_color_animation_effect: EffectNodeIndex::NONE,
                },
                &[],
                context,
                &[DisplayListInlineClip {
                    has_own_space: true,
                    space: clip_space,
                    ..rect_inline_clip(clip_rect)
                }],
            );
            display_list
        };
        let may_affect_viewport = |display_list: &TestDisplayList, rotation_nodes: &[SpatialNodeIndex]| {
            animated_content_may_affect_viewport(
                display_list,
                &display_list.runs,
                &tree,
                &[FloatPoint::default(), FloatPoint::default()],
                rotation_nodes,
                &[effect],
                viewport,
            )
        };
        let faded = ContextRef {
            effect,
            ..ContextRef::default()
        };

        // An animated opacity only affects the viewport where the clip lets the draw show.
        let box_on_screen = IntRect::new(0, 10, 10, 10);
        let box_off_screen = IntRect::new(0, 2000, 10, 10);
        assert!(may_affect_viewport(&draw_clipped_to(scroll, box_on_screen, faded), &[]));
        assert!(!may_affect_viewport(
            &draw_clipped_to(scroll, box_off_screen, faded),
            &[]
        ));

        // A clip in a rotating space moves the draw it clips, though the draw's own space stands still.
        assert!(may_affect_viewport(
            &draw_clipped_to(rotating, box_on_screen, ContextRef::default()),
            &[rotating]
        ));
    }

    #[test]
    fn damage_contains_old_and_new_command_bounds() {
        let tree = identity_tree();
        let old_display_list = fill_command_bytes(IntRect::new(10, 10, 20, 20), RED);
        let new_display_list = fill_command_bytes(IntRect::new(40, 40, 20, 20), BLUE);
        assert_eq!(
            damage(&old_display_list, &tree, &new_display_list, &tree),
            Some(IntRect::new(9, 9, 52, 52))
        );
    }

    #[test]
    fn changed_compositor_metadata_has_no_raster_damage() {
        let tree = identity_tree();
        let old_display_list = command_bytes(
            &CompositorMainThreadWheelEventRegion {
                rect: FloatRect::new(0.0, 0.0, 10.0, 10.0),
            },
            ContextRef::default(),
        );
        let new_display_list = command_bytes(
            &CompositorMainThreadWheelEventRegion {
                rect: FloatRect::new(20.0, 20.0, 10.0, 10.0),
            },
            ContextRef::default(),
        );
        assert_eq!(
            damage(&old_display_list, &tree, &new_display_list, &tree),
            Some(IntRect::default())
        );
    }

    fn compositor_scrollbar(is_painted_by_compositor: bool) -> CompositorScrollbar {
        CompositorScrollbar {
            scroll_node_index: SpatialNodeIndex(1),
            gutter_rect: IntRect::default(),
            thumb_rect: IntRect::new(98, 0, 2, 20),
            track_rect: IntRect::new(96, 0, 4, 100),
            expanded_gutter_rect: IntRect::new(92, 0, 8, 100),
            expanded_thumb_rect: IntRect::new(94, 0, 6, 20),
            scroll_size: 0.8,
            expanded_scroll_size: 0.8,
            min_scroll_offset: 0.0,
            max_scroll_offset: 100.0,
            thumb_color: RED,
            thumb_outline_color: Color::TRANSPARENT,
            track_color: Color::TRANSPARENT,
            vertical: true,
            is_painted_by_compositor,
        }
    }

    fn compositor_scrollbar_bytes(is_painted_by_compositor: bool) -> TestDisplayList {
        command_bytes(&compositor_scrollbar(is_painted_by_compositor), ContextRef::default())
    }

    #[test]
    fn moved_scrollbar_metadata_requires_full_repaint_only_when_the_compositor_paints_it() {
        let old_tree = identity_tree();
        let new_tree = VisualContextTree::create(transform(translation_matrix(10.0, 0.0, 0.0)));

        let painted_by_display_list = compositor_scrollbar_bytes(false);
        assert_eq!(
            damage(&painted_by_display_list, &old_tree, &painted_by_display_list, &new_tree),
            Some(IntRect::default())
        );

        let painted_by_compositor = compositor_scrollbar_bytes(true);
        assert_eq!(
            damage(&painted_by_compositor, &old_tree, &painted_by_compositor, &new_tree),
            None
        );
    }

    #[test]
    fn changed_visual_context_damages_affected_commands() {
        let old_tree = identity_tree();
        let new_tree = VisualContextTree::create(transform(translation_matrix(10.0, 0.0, 0.0)));
        let display_list = fill_command_bytes(IntRect::new(10, 10, 20, 20), RED);
        assert_eq!(
            damage(&display_list, &old_tree, &display_list, &new_tree),
            Some(IntRect::new(9, 9, 32, 22))
        );
    }

    #[test]
    fn changed_extent_affecting_filter_damages_its_old_and_new_output() {
        assert_eq!(
            damage_for_filter_change(blur_filter(1.0), blur_filter(10.0)),
            Some(IntRect::new(0, 0, 61, 61))
        );
        assert_eq!(
            damage_for_filter_change(drop_shadow_filter(1.0, 1.0), drop_shadow_filter(10.0, 10.0)),
            Some(IntRect::new(0, 0, 71, 71))
        );
        let flood = Filter::Flood {
            color: RED,
            opacity: 1.0,
        }
        .serialize();
        assert_eq!(damage_for_filter_change(blur_filter(1.0), flood), None);
    }

    #[test]
    fn changed_filter_clipped_outside_viewport_has_no_damage() {
        let make_scene = |radius, clip_y, clip_mode, clip_outside_filter| {
            let mut tree = identity_tree();
            let clip = tree.append_clip(
                ClipNodeData::Rect(ClipData {
                    rect: FloatRect::new(0.0, clip_y, 100.0, 100.0),
                    corner_radii: CornerRadii::default(),
                    mode: clip_mode,
                }),
                ClipNodeIndex::NONE,
                VISUAL_VIEWPORT_NODE_INDEX,
            );
            let effect = tree.append_effect(
                effects(blur_filter(radius)),
                EffectNodeIndex::NONE,
                VISUAL_VIEWPORT_NODE_INDEX,
                if clip_outside_filter { clip } else { ClipNodeIndex::NONE },
            );
            let context = ContextRef {
                clip,
                effect,
                ..ContextRef::default()
            };
            let rect = IntRect::new(0, 200, 20, 20);
            let fill = FillRect {
                rect,
                color: RED,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: EffectNodeIndex::NONE,
            };
            (command_bytes(&fill, context), tree)
        };
        let (old_bytes, old_tree) = make_scene(1.0, 200.0, ClipMode::Intersect, true);
        let (new_bytes, new_tree) = make_scene(200.0, 200.0, ClipMode::Intersect, true);
        assert_eq!(
            damage(&old_bytes, &old_tree, &new_bytes, &new_tree),
            Some(IntRect::default())
        );

        let whole_viewport = Some(IntRect::new(0, 0, 100, 100));

        // The filter can expand an inner clip's output back into view.
        let (old_bytes, old_tree) = make_scene(1.0, 200.0, ClipMode::Intersect, false);
        let (new_bytes, new_tree) = make_scene(200.0, 200.0, ClipMode::Intersect, false);
        assert_eq!(damage(&old_bytes, &old_tree, &new_bytes, &new_tree), whole_viewport);

        // Difference clips exclude their rect rather than bounding the effect's output.
        let (old_bytes, old_tree) = make_scene(1.0, 200.0, ClipMode::Difference, true);
        let (new_bytes, new_tree) = make_scene(200.0, 200.0, ClipMode::Difference, true);
        assert_eq!(damage(&old_bytes, &old_tree, &new_bytes, &new_tree), whole_viewport);

        // Output inside the viewport on either side is damaged.
        let (old_bytes, old_tree) = make_scene(100.0, 0.0, ClipMode::Intersect, true);
        let (new_bytes, new_tree) = make_scene(200.0, 200.0, ClipMode::Intersect, true);
        assert_eq!(damage(&old_bytes, &old_tree, &new_bytes, &new_tree), whole_viewport);
        assert_eq!(damage(&new_bytes, &new_tree, &old_bytes, &old_tree), whole_viewport);

        // An outer filter can expand the clipped inner filter back into the viewport.
        let (old_bytes, mut old_tree) = make_scene(1.0, 200.0, ClipMode::Intersect, true);
        let (new_bytes, mut new_tree) = make_scene(200.0, 200.0, ClipMode::Intersect, true);
        for tree in [&mut old_tree, &mut new_tree] {
            let parent = tree.append_effect(
                effects(blur_filter(300.0)),
                EffectNodeIndex::NONE,
                VISUAL_VIEWPORT_NODE_INDEX,
                ClipNodeIndex::NONE,
            );
            tree.effect_nodes[0].parent = parent;
        }
        assert_eq!(damage(&old_bytes, &old_tree, &new_bytes, &new_tree), whole_viewport);

        // A descendant escaping the clip widens the effect's output clip in the replay plan.
        let (mut old_bytes, old_tree) = make_scene(1.0, 200.0, ClipMode::Intersect, true);
        let (mut new_bytes, new_tree) = make_scene(200.0, 200.0, ClipMode::Intersect, true);
        let rect = IntRect::new(0, 0, 20, 20);
        let escaped = command_bytes(
            &FillRect {
                rect,
                color: RED,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: EffectNodeIndex::NONE,
            },
            ContextRef {
                effect: EffectNodeIndex(0),
                ..ContextRef::default()
            },
        );
        old_bytes.extend_from_slice(&escaped);
        new_bytes.extend_from_slice(&escaped);
        assert_eq!(damage(&old_bytes, &old_tree, &new_bytes, &new_tree), whole_viewport);
    }

    fn damage_for_fill_color_change_under(
        context_of: impl Fn(&mut VisualContextTree) -> ContextRef,
    ) -> Option<IntRect> {
        let mut tree = identity_tree();
        let context = context_of(&mut tree);
        let rect = IntRect::new(10, 10, 20, 20);
        let fill = |color| FillRect {
            rect,
            color,
            compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            background_color_animation_effect: EffectNodeIndex::NONE,
        };
        let old_bytes = command_bytes(&fill(RED), context);
        let new_bytes = command_bytes(&fill(BLUE), context);
        damage(&old_bytes, &tree, &new_bytes, &tree)
    }

    #[test]
    fn a_changed_command_damages_where_the_filters_above_it_spread_it() {
        let under_filter = |filter: Vec<u8>| {
            move |tree: &mut VisualContextTree| {
                context_in(
                    VISUAL_VIEWPORT_NODE_INDEX,
                    effect_context(tree, effects(filter.clone())),
                )
            }
        };
        assert_eq!(
            damage_for_fill_color_change_under(under_filter(blur_filter(2.0))),
            Some(IntRect::new(3, 3, 34, 34))
        );
        assert_eq!(
            damage_for_fill_color_change_under(under_filter(drop_shadow_filter(30.0, 0.0))),
            Some(IntRect::new(9, 9, 52, 52))
        );
        assert_eq!(
            damage_for_fill_color_change_under(under_filter(color_filter(0.5))),
            Some(IntRect::new(9, 9, 22, 22))
        );
        let flood = Filter::Flood {
            color: RED,
            opacity: 1.0,
        }
        .serialize();
        assert_eq!(damage_for_fill_color_change_under(under_filter(flood.clone())), None);

        // Spread content cannot show past the clips outside its filters.
        let under_clipped_filter = |filter: Vec<u8>, clip_rect: FloatRect| {
            move |tree: &mut VisualContextTree| {
                let clip = tree.append_clip(
                    ClipNodeData::rect_clip(clip_rect),
                    ClipNodeIndex::NONE,
                    VISUAL_VIEWPORT_NODE_INDEX,
                );
                let effect = tree.append_effect(
                    effects(filter.clone()),
                    EffectNodeIndex::NONE,
                    VISUAL_VIEWPORT_NODE_INDEX,
                    clip,
                );
                ContextRef {
                    clip,
                    effect,
                    ..ContextRef::default()
                }
            }
        };
        assert_eq!(
            damage_for_fill_color_change_under(under_clipped_filter(
                blur_filter(200.0),
                FloatRect::new(0.0, 0.0, 50.0, 50.0)
            )),
            Some(IntRect::new(0, 0, 52, 52))
        );
        assert_eq!(
            damage_for_fill_color_change_under(under_clipped_filter(
                flood.clone(),
                FloatRect::new(20.0, 20.0, 30.0, 30.0)
            )),
            Some(IntRect::new(18, 18, 34, 34))
        );
        assert_eq!(
            damage_for_fill_color_change_under(under_clipped_filter(flood, FloatRect::new(0.0, 200.0, 100.0, 100.0))),
            Some(IntRect::default())
        );
    }

    // A filter that keeps only where its source overlaps a shifted copy of itself shows a change to
    // one part of the source wherever the copy of an unchanged part overlaps it.
    #[test]
    fn a_change_masked_by_a_shifted_copy_of_the_source_damages_both_copies() {
        let mut tree = identity_tree();
        let masked_by_shifted_copy = Filter::Blend {
            background: Some(Box::new(Filter::Offset {
                dx: 40.0,
                dy: 0.0,
                input: None,
            })),
            foreground: None,
            mode: CompositingAndBlendingOperator::SourceIn,
        }
        .serialize();
        let context = context_in(
            VISUAL_VIEWPORT_NODE_INDEX,
            effect_context(&mut tree, effects(masked_by_shifted_copy)),
        );
        let scene = |right_color| {
            let fill = |x, color| {
                let rect = IntRect::new(x, 10, 20, 20);
                command_bytes(
                    &FillRect {
                        rect,
                        color,
                        compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                        background_color_animation_effect: EffectNodeIndex::NONE,
                    },
                    context,
                )
            };
            let mut bytes = fill(10, RED);
            bytes.extend_from_slice(&fill(50, right_color));
            bytes
        };
        assert_eq!(
            damage(&scene(RED), &tree, &scene(BLUE), &tree),
            Some(IntRect::new(49, 9, 51, 22))
        );
    }

    #[test]
    fn damage_beneath_a_backdrop_filter_spreads_into_its_region() {
        let region = IntRect::new(25, 0, 50, 50);
        let mut tree = identity_tree();
        let backdrop_context = context_in(
            VISUAL_VIEWPORT_NODE_INDEX,
            effect_context(&mut tree, backdrop_effects(blur_filter(4.0), region)),
        );
        let scene = |fill_rect, color| {
            let mut bytes = command_bytes(
                &FillRect {
                    rect: fill_rect,
                    color,
                    compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                    background_color_animation_effect: EffectNodeIndex::NONE,
                },
                ContextRef::default(),
            );
            bytes.extend_from_slice(&command_bytes(&BackdropFilterRegion { rect: region }, backdrop_context));
            bytes
        };
        let within_reach = IntRect::new(0, 10, 30, 20);
        assert_eq!(
            damage(&scene(within_reach, RED), &tree, &scene(within_reach, BLUE), &tree),
            Some(IntRect::new(0, 0, 76, 51))
        );
        let out_of_reach = IntRect::new(0, 10, 10, 20);
        assert_eq!(
            damage(&scene(out_of_reach, RED), &tree, &scene(out_of_reach, BLUE), &tree),
            Some(IntRect::new(0, 9, 11, 22))
        );
    }

    #[test]
    fn forced_damage_at_a_backdrop_filter_repaints_its_whole_region() {
        let region = IntRect::new(25, 0, 50, 50);
        let mut tree = identity_tree();
        let backdrop_context = context_in(
            VISUAL_VIEWPORT_NODE_INDEX,
            effect_context(&mut tree, backdrop_effects(blur_filter(4.0), region)),
        );
        let bytes = command_bytes(&BackdropFilterRegion { rect: region }, backdrop_context);
        let forced_damage = |forced_damage_rect| {
            damage_for_live_content_changes(
                &bytes,
                &tree,
                LiveContentChanges {
                    forced_damage_rect,
                    ..LiveContentChanges::default()
                },
            )
        };
        assert_eq!(forced_damage(IntRect::default()), Some(IntRect::default()));
        assert_eq!(
            forced_damage(IntRect::new(40, 10, 3, 20)),
            Some(IntRect::new(24, 0, 52, 51))
        );
        assert_eq!(
            forced_damage(IntRect::new(0, 10, 3, 20)),
            Some(IntRect::new(0, 9, 4, 22))
        );
    }

    #[test]
    fn a_damaged_backdrop_region_is_spread_by_the_filters_of_its_effect() {
        let region = IntRect::new(25, 0, 50, 50);
        let mut tree = identity_tree();
        let shadowed_backdrop_context = context_in(
            VISUAL_VIEWPORT_NODE_INDEX,
            effect_context(
                &mut tree,
                EffectNodeData::Effects(EffectsData {
                    isolation: false,
                    opacity: 1.0,
                    blend_mode: CompositingAndBlendingOperator::Normal,
                    filter: Some(std::sync::Arc::new(drop_shadow_filter(20.0, 0.0))),
                    backdrop_filter: Some(BackdropFilterData {
                        filter: std::sync::Arc::new(blur_filter(4.0)),
                        region,
                        corner_radii: CornerRadii::default(),
                    }),
                }),
            ),
        );
        let fill_rect = IntRect::new(0, 10, 30, 20);
        let scene = |color| {
            let mut bytes = command_bytes(
                &FillRect {
                    rect: fill_rect,
                    color,
                    compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                    background_color_animation_effect: EffectNodeIndex::NONE,
                },
                ContextRef::default(),
            );
            bytes.extend_from_slice(&command_bytes(
                &BackdropFilterRegion { rect: region },
                shadowed_backdrop_context,
            ));
            bytes
        };
        // The region and its shadow, 20 pixels down and right.
        assert_eq!(
            damage(&scene(RED), &tree, &scene(BLUE), &tree),
            Some(IntRect::new(0, 0, 96, 71))
        );
    }

    #[test]
    fn changed_color_filter_has_bounded_damage() {
        assert_eq!(
            damage_for_filter_change(color_filter(0.5), color_filter(1.0)),
            Some(IntRect::new(9, 9, 22, 22))
        );
    }

    fn backdrop_effects(filter: Vec<u8>, region: IntRect) -> EffectNodeData {
        EffectNodeData::Effects(EffectsData {
            isolation: false,
            opacity: 1.0,
            blend_mode: CompositingAndBlendingOperator::Normal,
            filter: None,
            backdrop_filter: Some(BackdropFilterData {
                filter: std::sync::Arc::new(filter),
                region,
                corner_radii: CornerRadii::default(),
            }),
        })
    }

    // A backdrop filter's output is limited to its region, which the region command records, so a
    // changed backdrop filter damages that region even when it widens the filter's extent.
    #[test]
    fn changed_backdrop_filter_damages_its_region() {
        let region = IntRect::new(10, 10, 20, 20);
        let mut old_tree = identity_tree();
        let old_context = effect_context(&mut old_tree, backdrop_effects(blur_filter(1.0), region));
        let mut new_tree = identity_tree();
        let new_context = effect_context(&mut new_tree, backdrop_effects(blur_filter(10.0), region));
        let mut unchanged_tree = identity_tree();
        let unchanged_context = effect_context(&mut unchanged_tree, backdrop_effects(blur_filter(1.0), region));
        let marker = BackdropFilterRegion { rect: region };
        let old_display_list = command_bytes(&marker, context_in(VISUAL_VIEWPORT_NODE_INDEX, old_context));
        let new_display_list = command_bytes(&marker, context_in(VISUAL_VIEWPORT_NODE_INDEX, new_context));
        let unchanged_display_list = command_bytes(&marker, context_in(VISUAL_VIEWPORT_NODE_INDEX, unchanged_context));
        assert_eq!(
            damage(&old_display_list, &old_tree, &new_display_list, &new_tree),
            Some(IntRect::new(9, 9, 22, 22))
        );
        assert_eq!(
            damage(&old_display_list, &old_tree, &unchanged_display_list, &unchanged_tree),
            Some(IntRect::default())
        );
    }

    #[test]
    fn commands_under_an_empty_effective_clip_do_not_damage() {
        let mut old_tree = identity_tree();
        let old_context = clip_context(&mut old_tree, clip(FloatRect::default()));
        let mut new_tree = identity_tree();
        let new_context = clip_context(&mut new_tree, clip(FloatRect::default()));
        let old_display_list = command_bytes(
            &FillRect {
                rect: IntRect::new(10, 10, 20, 20),
                color: RED,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: EffectNodeIndex::NONE,
            },
            context_in(VISUAL_VIEWPORT_NODE_INDEX, old_context),
        );
        let new_display_list = command_bytes(
            &FillRect {
                rect: IntRect::new(30, 30, 20, 20),
                color: BLUE,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: EffectNodeIndex::NONE,
            },
            context_in(VISUAL_VIEWPORT_NODE_INDEX, new_context),
        );
        assert_eq!(
            damage(&old_display_list, &old_tree, &new_display_list, &new_tree),
            Some(IntRect::default())
        );
    }

    #[test]
    fn a_clip_growing_from_empty_damages_the_commands_it_reveals() {
        let mut old_tree = identity_tree();
        let old_context = clip_context(&mut old_tree, clip(FloatRect::default()));
        let mut new_tree = identity_tree();
        let new_context = clip_context(&mut new_tree, clip(FloatRect::new(0.0, 0.0, 100.0, 100.0)));
        let fill = FillRect {
            rect: IntRect::new(10, 10, 20, 20),
            color: RED,
            compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            background_color_animation_effect: EffectNodeIndex::NONE,
        };
        let old_display_list = command_bytes(&fill, context_in(VISUAL_VIEWPORT_NODE_INDEX, old_context));
        let new_display_list = command_bytes(&fill, context_in(VISUAL_VIEWPORT_NODE_INDEX, new_context));
        assert_eq!(
            damage(&old_display_list, &old_tree, &new_display_list, &new_tree),
            Some(IntRect::new(9, 9, 22, 22))
        );
    }

    #[test]
    fn unrelated_inserted_visual_context_does_not_damage_commands() {
        let mut old_tree = identity_tree();
        let old_command_spatial = old_tree.append_spatial(
            SpatialData::Transform(transform(FloatMatrix4x4::identity())),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let mut new_tree = identity_tree();
        effect_context(
            &mut new_tree,
            EffectNodeData::Effects(EffectsData {
                isolation: false,
                opacity: 0.5,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
                backdrop_filter: None,
            }),
        );
        let new_command_spatial = new_tree.append_spatial(
            SpatialData::Transform(transform(FloatMatrix4x4::identity())),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let fill = FillRect {
            rect: IntRect::new(10, 10, 20, 20),
            color: RED,
            compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            background_color_animation_effect: EffectNodeIndex::NONE,
        };
        let old_display_list = command_bytes(&fill, context_in(old_command_spatial, ContextRef::default()));
        let new_display_list = command_bytes(&fill, context_in(new_command_spatial, ContextRef::default()));
        assert_eq!(
            damage(&old_display_list, &old_tree, &new_display_list, &new_tree),
            Some(IntRect::default())
        );
    }

    #[test]
    fn canvas_without_new_content_is_not_damaged() {
        let tree = identity_tree();
        let canvas = canvas_command_bytes(IntRect::new(10, 10, 20, 20));
        let old_fill = fill_command_bytes(IntRect::new(50, 50, 10, 10), RED);
        let new_fill = fill_command_bytes(IntRect::new(50, 50, 10, 10), BLUE);
        let mut old_display_list = canvas.clone();
        old_display_list.extend_from_slice(&old_fill);
        let mut new_display_list = canvas;
        new_display_list.extend_from_slice(&new_fill);
        assert_eq!(
            damage(&old_display_list, &tree, &new_display_list, &tree),
            Some(IntRect::new(49, 49, 12, 12))
        );
    }

    #[test]
    fn inserted_and_removed_chunks_do_not_damage_shifted_chunks() {
        let tree = identity_tree();
        let first = fill_command_bytes(IntRect::new(10, 10, 10, 10), RED);
        let second = fill_command_bytes(IntRect::new(30, 10, 10, 10), GREEN);
        let third = fill_command_bytes(IntRect::new(50, 10, 10, 10), BLUE);
        let removed = fill_command_bytes(IntRect::new(70, 10, 10, 10), YELLOW);
        let inserted = fill_command_bytes(IntRect::new(20, 40, 10, 10), CYAN);
        let mut old_display_list = first.clone();
        old_display_list.extend_from_slice(&second);
        old_display_list.extend_from_slice(&third);
        old_display_list.extend_from_slice(&removed);
        let mut new_display_list = first;
        new_display_list.extend_from_slice(&inserted);
        new_display_list.extend_from_slice(&second);
        new_display_list.extend_from_slice(&third);
        assert_eq!(
            damage(&old_display_list, &tree, &new_display_list, &tree),
            Some(IntRect::new(19, 9, 62, 42))
        );
    }

    #[test]
    fn clip_paths_are_compared_by_content_across_allocations() {
        let rect = IntRect::new(10, 10, 20, 20);
        let scene = |apex_x: f32| {
            let mut path = Path::new();
            path.move_to(FloatPoint { x: 10.0, y: 30.0 });
            path.line_to(FloatPoint { x: apex_x, y: 10.0 });
            path.line_to(FloatPoint { x: 30.0, y: 30.0 });
            path.close();
            let mut tree = identity_tree();
            let context = clip_context(
                &mut tree,
                ClipNodeData::Path(ClipPathData {
                    path: std::sync::Arc::new(path),
                    bounding_rect: rect,
                    fill_rule: WindingRule::Nonzero,
                }),
            );
            let fill = FillRect {
                rect,
                color: RED,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: EffectNodeIndex::NONE,
            };
            (
                command_bytes(&fill, context_in(VISUAL_VIEWPORT_NODE_INDEX, context)),
                tree,
            )
        };
        let (old_display_list, old_tree) = scene(20.0);
        let (same_display_list, same_tree) = scene(20.0);
        assert_eq!(
            damage(&old_display_list, &old_tree, &same_display_list, &same_tree),
            Some(IntRect::default())
        );
        let (changed_display_list, changed_tree) = scene(25.0);
        assert_eq!(
            damage(&old_display_list, &old_tree, &changed_display_list, &changed_tree),
            Some(IntRect::new(9, 9, 22, 22))
        );
    }

    #[test]
    fn damage_covering_the_viewport_ends_the_diff() {
        let viewport = IntRect::new(0, 0, 100, 100);
        // The cover's run comes first, and moving it damages the whole viewport.
        let scene = |cover_offset, filter| {
            let mut tree = identity_tree();
            let cover_spatial = tree.append_spatial(
                SpatialData::Transform(transform(translation_matrix(cover_offset, 0.0, 0.0))),
                VISUAL_VIEWPORT_NODE_INDEX,
            );
            let filtered_context = context_in(VISUAL_VIEWPORT_NODE_INDEX, effect_context(&mut tree, effects(filter)));
            let fill = |rect| FillRect {
                rect,
                color: BLUE,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: EffectNodeIndex::NONE,
            };
            let filtered_rect = IntRect::new(10, 10, 20, 20);
            let mut bytes = command_bytes(&fill(viewport), context_in(cover_spatial, ContextRef::default()));
            bytes.extend_from_slice(&command_bytes(&fill(filtered_rect), filtered_context));
            (bytes, tree)
        };
        let flood = || {
            Filter::Flood {
                color: RED,
                opacity: 1.0,
            }
            .serialize()
        };
        let (old_bytes, old_tree) = scene(0.0, blur_filter(1.0));
        let (flood_bytes, flood_tree) = scene(0.0, flood());
        let (flood_and_moved_cover_bytes, flood_and_moved_cover_tree) = scene(1.0, flood());
        // A flood alone requires a full repaint; a covering change reported first means the same
        // thing.
        assert_eq!(damage(&old_bytes, &old_tree, &flood_bytes, &flood_tree), None);
        assert_eq!(
            damage(
                &old_bytes,
                &old_tree,
                &flood_and_moved_cover_bytes,
                &flood_and_moved_cover_tree
            ),
            Some(viewport)
        );
    }

    fn fill(rect: IntRect, color: Color) -> FillRect {
        FillRect {
            rect,
            color,
            compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            background_color_animation_effect: EffectNodeIndex::NONE,
        }
    }

    fn fill_in(rect: IntRect, color: Color, context: ContextRef) -> TestDisplayList {
        command_bytes(&fill(rect, color), context)
    }

    fn scroll_data() -> SpatialData {
        SpatialData::Scroll(ScrollData {
            state_slot: NO_SCROLL_STATE_SLOT,
            owner_paintable: NodeSlotId::INVALID,
        })
    }

    fn identity_transform() -> SpatialData {
        SpatialData::Transform(transform(FloatMatrix4x4::identity()))
    }

    #[test]
    fn a_replaced_chunk_between_unchanged_ones_damages_only_what_both_versions_draw() {
        let tree = identity_tree();
        let unchanged = |x| fill_command_bytes(IntRect::new(x, 60, 10, 10), GREEN);
        let scene = |color| {
            list_of(&[
                &unchanged(0),
                &unchanged(20),
                &fill_command_bytes(IntRect::new(10, 10, 10, 10), color),
                &unchanged(40),
                &unchanged(60),
            ])
        };
        assert_eq!(
            damage(&scene(RED), &tree, &scene(BLUE), &tree),
            Some(IntRect::new(9, 9, 12, 12))
        );
    }

    #[test]
    fn unchanged_chunks_between_two_changed_ones_are_not_damaged() {
        let tree = identity_tree();
        let unchanged: Vec<TestDisplayList> = (0..8)
            .map(|index| fill_command_bytes(IntRect::new(10 * index, 60, 10, 10), GREEN))
            .collect();
        let scene = |color| {
            let mut list = fill_command_bytes(IntRect::new(0, 0, 10, 10), color);
            for chunk in &unchanged {
                list.extend_from_slice(chunk);
            }
            list.extend_from_slice(&fill_command_bytes(IntRect::new(10, 0, 10, 10), color));
            list
        };
        assert_eq!(
            damage(&scene(RED), &tree, &scene(BLUE), &tree),
            Some(IntRect::new(0, 0, 21, 11))
        );
    }

    #[test]
    fn a_chunk_moved_past_others_is_damaged_where_it_is_drawn_and_the_others_are_not() {
        let tree = identity_tree();
        let moved = fill_command_bytes(IntRect::new(10, 10, 10, 10), RED);
        let first = fill_command_bytes(IntRect::new(50, 50, 10, 10), GREEN);
        let second = fill_command_bytes(IntRect::new(70, 50, 10, 10), BLUE);
        let moved_damage = Some(IntRect::new(9, 9, 12, 12));
        assert_eq!(
            damage(
                &list_of(&[&moved, &first, &second]),
                &tree,
                &list_of(&[&first, &second, &moved]),
                &tree
            ),
            moved_damage
        );
        assert_eq!(
            damage(
                &list_of(&[&first, &second, &moved]),
                &tree,
                &list_of(&[&moved, &first, &second]),
                &tree
            ),
            moved_damage
        );
        assert_eq!(
            damage(&list_of(&[&moved, &first]), &tree, &list_of(&[&first, &moved]), &tree),
            moved_damage
        );
    }

    #[test]
    fn chunks_of_one_name_pair_up_in_paint_order() {
        let tree = identity_tree();
        let repeated = fill_command_bytes(IntRect::new(50, 50, 10, 10), GREEN);
        let between = fill_command_bytes(IntRect::new(70, 50, 10, 10), BLUE);
        let edge = |x, color| fill_command_bytes(IntRect::new(x, 10, 10, 10), color);
        assert_eq!(
            damage(
                &list_of(&[&edge(0, RED), &repeated, &between, &repeated, &edge(20, RED)]),
                &tree,
                &list_of(&[&edge(0, YELLOW), &repeated, &between, &repeated, &edge(20, CYAN)]),
                &tree
            ),
            Some(IntRect::new(0, 9, 31, 12))
        );
        assert_eq!(
            damage(
                &list_of(&[&repeated, &between, &repeated]),
                &tree,
                &list_of(&[&repeated, &repeated]),
                &tree
            ),
            Some(IntRect::new(69, 49, 12, 12))
        );
        assert_eq!(
            damage(
                &list_of(&[&repeated, &between]),
                &tree,
                &list_of(&[&repeated, &between, &repeated]),
                &tree
            ),
            Some(IntRect::new(49, 49, 12, 12))
        );
    }

    #[test]
    fn a_scroll_offset_change_damages_what_the_scroller_moves_and_nothing_else() {
        let mut tree = identity_tree();
        let scroller = tree.append_spatial(scroll_data(), VISUAL_VIEWPORT_NODE_INDEX);
        let scrolled = tree.append_spatial(identity_transform(), scroller);
        let list = list_of(&[
            &fill_in(
                IntRect::new(10, 10, 10, 10),
                RED,
                context_in(scrolled, ContextRef::default()),
            ),
            &fill_command_bytes(IntRect::new(60, 60, 10, 10), GREEN),
        ]);
        let unscrolled = vec![FloatPoint::default(); tree.spatial_nodes.len()];
        let mut scrolled_down = unscrolled.clone();
        scrolled_down[scroller.0 as usize] = FloatPoint { x: 0.0, y: 20.0 };
        let viewport = IntRect::new(0, 0, 100, 100);
        assert_eq!(
            damage_with_scroll_offsets(&list, &tree, &unscrolled, &list, &tree, &unscrolled, viewport),
            Some(IntRect::default())
        );
        assert_eq!(
            damage_with_scroll_offsets(&list, &tree, &unscrolled, &list, &tree, &scrolled_down, viewport),
            Some(IntRect::new(9, 9, 12, 32))
        );
    }

    #[test]
    fn a_change_reaches_a_child_stored_below_its_parent() {
        let mut tree = identity_tree();
        let child = tree.append_spatial(identity_transform(), VISUAL_VIEWPORT_NODE_INDEX);
        let scroller = tree.append_spatial(scroll_data(), VISUAL_VIEWPORT_NODE_INDEX);
        tree.spatial_nodes[child.0 as usize].parent = scroller;
        let list = fill_in(
            IntRect::new(10, 10, 10, 10),
            RED,
            context_in(child, ContextRef::default()),
        );
        let unscrolled = vec![FloatPoint::default(); tree.spatial_nodes.len()];
        let mut scrolled_down = unscrolled.clone();
        scrolled_down[scroller.0 as usize] = FloatPoint { x: 0.0, y: 20.0 };
        assert_eq!(
            damage_with_scroll_offsets(
                &list,
                &tree,
                &unscrolled,
                &list,
                &tree.clone(),
                &scrolled_down,
                IntRect::new(0, 0, 100, 100)
            ),
            Some(IntRect::new(9, 9, 12, 32))
        );
    }

    #[test]
    fn a_node_given_another_parent_damages_what_it_draws() {
        let tree_with_child_of_shifted_node = |child_of_shifted_node: bool| {
            let mut tree = identity_tree();
            let shifted = tree.append_spatial(
                SpatialData::Transform(transform(translation_matrix(30.0, 0.0, 0.0))),
                VISUAL_VIEWPORT_NODE_INDEX,
            );
            let child = tree.append_spatial(
                identity_transform(),
                if child_of_shifted_node {
                    shifted
                } else {
                    VISUAL_VIEWPORT_NODE_INDEX
                },
            );
            (tree, child)
        };
        let (old_tree, child) = tree_with_child_of_shifted_node(false);
        let (new_tree, _) = tree_with_child_of_shifted_node(true);
        let list = fill_in(
            IntRect::new(10, 10, 10, 10),
            RED,
            context_in(child, ContextRef::default()),
        );
        assert_eq!(
            damage(&list, &old_tree, &list, &new_tree),
            Some(IntRect::new(9, 9, 42, 12))
        );
    }

    #[test]
    fn a_slot_that_holds_another_kind_of_node_damages_what_it_draws_even_where_it_draws_alike() {
        let mut old_tree = identity_tree();
        let node = old_tree.append_spatial(identity_transform(), VISUAL_VIEWPORT_NODE_INDEX);
        let mut new_tree = identity_tree();
        new_tree.append_spatial(scroll_data(), VISUAL_VIEWPORT_NODE_INDEX);
        let list = fill_in(
            IntRect::new(10, 10, 10, 10),
            RED,
            context_in(node, ContextRef::default()),
        );
        assert_eq!(
            damage(&list, &old_tree, &list, &new_tree),
            Some(IntRect::new(9, 9, 12, 12))
        );
    }

    #[test]
    fn a_clip_placed_in_another_space_damages_what_it_clips() {
        let tree_with_clip_in_shifted_space = |in_shifted_space: bool| {
            let mut tree = identity_tree();
            let shifted = tree.append_spatial(
                SpatialData::Transform(transform(translation_matrix(0.0, 5.0, 0.0))),
                VISUAL_VIEWPORT_NODE_INDEX,
            );
            clip_context(&mut tree, clip(FloatRect::new(0.0, 0.0, 100.0, 100.0)));
            if in_shifted_space {
                tree.clip_nodes[0].spatial = shifted;
            }
            tree
        };
        let (old_tree, new_tree) = (
            tree_with_clip_in_shifted_space(false),
            tree_with_clip_in_shifted_space(true),
        );
        let list = fill_in(
            IntRect::new(10, 10, 10, 10),
            RED,
            ContextRef {
                clip: ClipNodeIndex(0),
                ..ContextRef::default()
            },
        );
        assert_eq!(
            damage(&list, &old_tree, &list, &tree_with_clip_in_shifted_space(false)),
            Some(IntRect::default())
        );
        assert_eq!(
            damage(&list, &old_tree, &list, &new_tree),
            Some(IntRect::new(9, 9, 12, 12))
        );
    }

    #[test]
    fn an_effect_that_starts_in_another_clip_damages_its_content() {
        let tree_with_effect_starting_in_second_clip = |starts_in_second_clip: bool| {
            let mut tree = identity_tree();
            let first_clip = tree.append_clip(
                clip(FloatRect::new(0.0, 0.0, 100.0, 100.0)),
                ClipNodeIndex::NONE,
                VISUAL_VIEWPORT_NODE_INDEX,
            );
            let second_clip = tree.append_clip(
                clip(FloatRect::new(0.0, 0.0, 100.0, 100.0)),
                ClipNodeIndex::NONE,
                VISUAL_VIEWPORT_NODE_INDEX,
            );
            let effect = tree.append_effect(
                EffectNodeData::Effects(EffectsData {
                    isolation: false,
                    opacity: 0.5,
                    blend_mode: CompositingAndBlendingOperator::Normal,
                    filter: None,
                    backdrop_filter: None,
                }),
                EffectNodeIndex::NONE,
                VISUAL_VIEWPORT_NODE_INDEX,
                if starts_in_second_clip { second_clip } else { first_clip },
            );
            (tree, effect)
        };
        let (old_tree, effect) = tree_with_effect_starting_in_second_clip(false);
        let (new_tree, _) = tree_with_effect_starting_in_second_clip(true);
        let list = fill_in(
            IntRect::new(10, 10, 10, 10),
            RED,
            ContextRef {
                effect,
                ..ContextRef::default()
            },
        );
        assert_eq!(
            damage(&list, &old_tree, &list, &new_tree),
            Some(IntRect::new(9, 9, 12, 12))
        );
    }

    fn mask_content_declaration(content: &TestDisplayList, rect: IntRect, context: ContextRef) -> TestDisplayList {
        let mut bytes = TestDisplayList::default();
        append_record(
            &mut bytes,
            &DeclareMaskContent {
                rect,
                effect: context.effect,
                content: DisplayListDataSpan {
                    offset: std::mem::size_of::<DeclareMaskContent>() as u32,
                    size: content.len() as u32,
                },
            },
            content,
            context,
            &[],
        );
        bytes.end_chunk();
        bytes
    }

    #[test]
    fn a_mask_whose_declaring_chunk_changed_damages_everything_it_masks() {
        let mut tree = identity_tree();
        let masked = effect_context(&mut tree, mask(IntRect::new(0, 0, 100, 100)));
        let declared_rect = IntRect::new(10, 10, 10, 10);
        let scene = |declared_color| {
            list_of(&[
                &mask_content_declaration(&fill_in(declared_rect, declared_color, masked), declared_rect, masked),
                &fill_in(IntRect::new(60, 60, 10, 10), BLUE, masked),
                &fill_command_bytes(IntRect::new(80, 10, 10, 10), GREEN),
            ])
        };
        assert_eq!(
            damage(&scene(RED), &tree, &scene(RED), &tree.clone()),
            Some(IntRect::default())
        );
        assert_eq!(
            damage(&scene(RED), &tree, &scene(GREEN), &tree),
            Some(IntRect::new(9, 9, 62, 62))
        );
    }

    #[test]
    fn a_mask_holding_text_is_unchanged_while_its_declaring_chunk_is() {
        let tree_with_mask = || {
            let mut tree = identity_tree();
            let masked = effect_context(&mut tree, mask(IntRect::new(0, 0, 100, 100)));
            (tree, masked)
        };
        let (old_tree, masked) = tree_with_mask();
        let (new_tree, _) = tree_with_mask();
        let text_rect = IntRect::new(10, 10, 20, 20);
        let text = command_bytes(
            &DrawGlyphRun {
                stroke_width: 0.0,
                font_smoothing: 0,
                font_id: FontResourceId(1),
                glyphs: DisplayListDataSpan::default(),
                rect: text_rect,
                glyph_bounding_rect: text_rect,
                translation: FloatPoint { x: 10.0, y: 10.0 },
                scale: 1.0,
                color: RED,
                orientation: Orientation::Horizontal,
            },
            masked,
        );
        let list = list_of(&[
            &mask_content_declaration(&text, text_rect, masked),
            &fill_in(IntRect::new(60, 60, 10, 10), BLUE, masked),
        ]);
        assert_eq!(damage(&list, &old_tree, &list, &new_tree), Some(IntRect::default()));
    }

    #[test]
    fn a_mask_whose_declaration_moved_damages_everything_it_masks() {
        let tree_with_declaration_shifted_by = |shift: f32| {
            let mut tree = identity_tree();
            let declaration_space = tree.append_spatial(
                SpatialData::Transform(transform(translation_matrix(shift, 0.0, 0.0))),
                VISUAL_VIEWPORT_NODE_INDEX,
            );
            let masked = effect_context(&mut tree, mask(IntRect::new(0, 0, 100, 100)));
            (tree, declaration_space, masked)
        };
        let (old_tree, declaration_space, masked) = tree_with_declaration_shifted_by(0.0);
        let (new_tree, _, _) = tree_with_declaration_shifted_by(5.0);
        let declared_rect = IntRect::new(10, 10, 10, 10);
        let declaration_context = context_in(declaration_space, masked);
        let list = list_of(&[
            &mask_content_declaration(
                &fill_in(declared_rect, RED, declaration_context),
                declared_rect,
                declaration_context,
            ),
            &fill_in(IntRect::new(60, 60, 10, 10), BLUE, masked),
        ]);
        assert_eq!(
            damage(&list, &old_tree, &list, &new_tree),
            Some(IntRect::new(9, 9, 62, 62))
        );
    }

    #[test]
    fn a_changed_chunk_with_a_scrollbar_the_compositor_paints_needs_a_full_repaint_only_where_it_moved() {
        let rect = IntRect::new(10, 10, 20, 20);
        let scene = |color| {
            let mut list = TestDisplayList::default();
            append_record(&mut list, &compositor_scrollbar(true), &[], ContextRef::default(), &[]);
            append_record(&mut list, &fill(rect, color), &[], ContextRef::default(), &[]);
            list
        };
        let moved_tree = VisualContextTree::create(transform(translation_matrix(10.0, 0.0, 0.0)));
        assert_eq!(
            damage(&scene(RED), &identity_tree(), &scene(BLUE), &identity_tree()),
            Some(IntRect::new(9, 9, 22, 22))
        );
        assert_eq!(damage(&scene(RED), &identity_tree(), &scene(BLUE), &moved_tree), None);
    }

    #[test]
    fn a_frame_of_the_same_list_tree_and_scroll_offsets_is_damaged_only_where_that_is_forced() {
        let tree = identity_tree();
        let list = list_of(&[
            &fill_command_bytes(IntRect::new(10, 10, 20, 20), RED),
            &fill_command_bytes(IntRect::new(50, 50, 20, 20), BLUE),
        ]);
        assert_eq!(
            damage_for_live_content_changes(&list, &tree, LiveContentChanges::default()),
            Some(IntRect::default())
        );
        assert_eq!(
            damage_for_live_content_changes(
                &list,
                &tree,
                LiveContentChanges {
                    forced_damage_rect: IntRect::new(5, 5, 5, 5),
                    ..LiveContentChanges::default()
                }
            ),
            Some(IntRect::new(4, 4, 7, 7))
        );
    }
}
