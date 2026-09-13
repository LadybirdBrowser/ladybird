/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::display_list::commands::{
    ClipNodeIndex, DisplayListCommandRun, EffectNodeIndex, ReplayClip, ReplayLayer, ReplayMask, SpatialNodeIndex,
};
use crate::painting::display_list::depth_sorted_plan::{DepthSortedReplayStepKind, build_depth_sorted_replay_plan};
use crate::painting::display_list::effect_clip_plan::EffectClipPlan;
use crate::painting::visual_context::queries::TreeCullingScratch;
use crate::painting::visual_context::{
    ClipNodeData, ContextRef, EffectNodeData, SpatialData, VisualContextTree, device_offset_for_index,
    resolve_leaf_to_context_matrices, should_cull_back_face,
};
use libgfx_rust::path::OwnedPath;
use libgfx_rust::{CornerRadii, FloatMatrix4x4, FloatPoint, FloatVector3, IntRect, WindingRule, translation_matrix};
use std::cell::RefCell;

pub trait ReplayPainter {
    fn canvas_matrix(&mut self) -> FloatMatrix4x4;
    fn set_matrix(&mut self, matrix: &FloatMatrix4x4);
    fn would_be_fully_clipped_by_painter(&mut self, rect: IntRect) -> bool;
    fn push_clip(&mut self, clip: &ReplayClip);
    fn push_clip_path(&mut self, path: &OwnedPath, winding_rule: WindingRule);
    fn push_layer(&mut self, layer: &ReplayLayer);
    fn push_mask(&mut self, mask: &ReplayMask);
    fn pop_mask(&mut self, mask: &ReplayMask, effect: EffectNodeIndex);
    fn pop(&mut self);
    fn push_device_space_plane_clip(&mut self, vertices: &[FloatVector3]);
    fn execute_run(&mut self, run_index: usize);
}

// Cumulative to-root matrices for every spatial node, resolved against the live scroll offsets
// and folded onto the canvas matrix at replay entry, so any node's space can be entered
// absolutely with a single set_matrix(). Spatial nodes therefore never touch the canvas save
// stack; only clips and effects do. A backface marker's entry carries the flattened matrix that
// feeds its cull test and its descendants, while content recorded directly under the marker
// belongs to its parent's plane, so draw_space redirects the marker to the parent's entry.
#[derive(Default)]
struct ReplayPaletteStorage {
    to_root_matrices: Vec<FloatMatrix4x4>,
    local_matrices: Vec<FloatMatrix4x4>,
    draw_spaces: Vec<SpatialNodeIndex>,
    backface_culled: Vec<bool>,
    flattens_inherited_transform: Vec<bool>,
}

// One entry of the canvas save stack: a clip or an effect the replay has entered.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Applied {
    Clip(ClipNodeIndex),
    Effect(EffectNodeIndex),
}

// Steady-state replays reuse the previous frame's capacity. Taking the storage out of the slot
// for the duration of a replay keeps re-entrant nested replays from clobbering the outer
// driver's buffers; the slot keeps whichever returned storage has the larger palette.
#[derive(Default)]
struct ReplayScratchStorage {
    palette: ReplayPaletteStorage,
    culling: TreeCullingScratch,
    applied: Vec<Applied>,
    target: Vec<Applied>,
}

thread_local! {
    static WARM_REPLAY_SCRATCH_STORAGE: RefCell<Option<ReplayScratchStorage>> = const { RefCell::new(None) };
}

fn take_replay_scratch_storage() -> ReplayScratchStorage {
    WARM_REPLAY_SCRATCH_STORAGE.with(|slot| slot.borrow_mut().take().unwrap_or_default())
}

fn return_replay_scratch_storage(mut storage: ReplayScratchStorage) {
    storage.palette.to_root_matrices.clear();
    storage.palette.local_matrices.clear();
    storage.palette.draw_spaces.clear();
    storage.palette.backface_culled.clear();
    storage.palette.flattens_inherited_transform.clear();
    storage.culling.clear();
    storage.applied.clear();
    storage.target.clear();
    WARM_REPLAY_SCRATCH_STORAGE.with(|slot| {
        let mut slot = slot.borrow_mut();
        let slot_has_larger_palette = slot.as_ref().is_some_and(|kept| {
            kept.palette.to_root_matrices.capacity() >= storage.palette.to_root_matrices.capacity()
        });
        if !slot_has_larger_palette {
            *slot = Some(storage);
        }
    });
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum SwitchResult {
    Switched,
    CulledByEffect,
}

struct ReplayDriver<'a, Painter: ReplayPainter> {
    tree: &'a VisualContextTree,
    command_runs: &'a [DisplayListCommandRun],
    effect_clips: &'a EffectClipPlan,
    painter: &'a mut Painter,
    palette: ReplayPaletteStorage,
    culling: TreeCullingScratch,
    spatial_dependency_order: Vec<u32>,
    tree_has_sorting_contexts: bool,
    replay_base_matrix: FloatMatrix4x4,
    // The palette entry the canvas matrix currently equals, if known; popping a clip or a layer
    // resets the matrix to its save point, so unwinding invalidates it. Recorded streams contain
    // no matrix-mutating commands, so playing commands never invalidates the cache.
    current_ctm_space: Option<SpatialNodeIndex>,
    // The canvas save stack as clips and effects, the stack a target context needs, and the
    // pair the applied stack currently serves.
    applied: Vec<Applied>,
    target: Vec<Applied>,
    applied_context: Option<(ClipNodeIndex, EffectNodeIndex)>,
}

fn replay_clip_of(clip: &crate::painting::visual_context::ClipData) -> ReplayClip {
    ReplayClip {
        rect: clip.rect,
        corner_radii: clip.corner_radii,
        mode: clip.mode,
    }
}

fn replay_layer_of(effects: &crate::painting::visual_context::EffectsData, effect: EffectNodeIndex) -> ReplayLayer {
    let (filter_bytes, filter_bytes_size) = match &effects.filter {
        Some(bytes) => (bytes.as_ptr(), bytes.len()),
        None => (std::ptr::null(), 0),
    };
    let (backdrop_filter_bytes, backdrop_filter_bytes_size, backdrop_region, backdrop_corner_radii) =
        match &effects.backdrop_filter {
            Some(backdrop) => (
                backdrop.filter.as_ptr(),
                backdrop.filter.len(),
                backdrop.region,
                backdrop.corner_radii,
            ),
            None => (std::ptr::null(), 0, IntRect::default(), CornerRadii::default()),
        };
    ReplayLayer {
        opacity: effects.opacity,
        blend_mode: effects.blend_mode,
        filter_bytes,
        filter_bytes_size,
        backdrop_filter_bytes,
        backdrop_filter_bytes_size,
        backdrop_region,
        backdrop_corner_radii,
        effect,
    }
}

fn replay_mask_of(mask: &crate::painting::visual_context::MaskData) -> ReplayMask {
    ReplayMask {
        rect: mask.rect,
        kind: mask.kind,
    }
}

fn fill_replay_palette_in_dependency_order(
    tree: &VisualContextTree,
    spatial_dependency_order: &[u32],
    scroll_offsets: &[FloatPoint],
    replay_base_matrix: FloatMatrix4x4,
    palette: &mut ReplayPaletteStorage,
) {
    let spatial_nodes = &tree.spatial_nodes;
    palette.to_root_matrices.clear();
    palette.to_root_matrices.resize(spatial_nodes.len(), replay_base_matrix);
    palette.local_matrices.clear();
    palette
        .local_matrices
        .resize(spatial_nodes.len(), FloatMatrix4x4::identity());
    palette.draw_spaces.clear();
    palette
        .draw_spaces
        .extend((0..spatial_nodes.len()).map(|index| SpatialNodeIndex(index as u32)));
    palette.backface_culled.clear();
    palette.backface_culled.resize(spatial_nodes.len(), false);
    palette.flattens_inherited_transform.clear();
    palette.flattens_inherited_transform.resize(spatial_nodes.len(), false);
    for &index in spatial_dependency_order {
        let i = index as usize;
        let node = &spatial_nodes[i];
        let parent = node.parent.0 as usize;
        let write_spatial =
            |palette: &mut ReplayPaletteStorage, local_matrix: FloatMatrix4x4, flattens_inherited_transform: bool| {
                let parent_matrix = if i == 0 {
                    replay_base_matrix
                } else {
                    palette.to_root_matrices[parent]
                };
                let inherited = if flattens_inherited_transform {
                    parent_matrix.flattened()
                } else {
                    parent_matrix
                };
                palette.to_root_matrices[i] = inherited.multiplied(local_matrix);
                palette.local_matrices[i] = local_matrix;
                palette.flattens_inherited_transform[i] = flattens_inherited_transform;
                palette.draw_spaces[i] = SpatialNodeIndex(index);
                palette.backface_culled[i] = if i == 0 { false } else { palette.backface_culled[parent] };
            };
        let write_spatial_translation = |palette: &mut ReplayPaletteStorage, offset: FloatPoint| {
            // Whole device pixels, so scrolled content never lands on subpixel positions.
            write_spatial(
                palette,
                translation_matrix(offset.x as i32 as f32, offset.y as i32 as f32, 0.0),
                false,
            );
        };
        match &node.data {
            SpatialData::Transform(transform) => {
                write_spatial(
                    palette,
                    transform.matrix_including_origin(),
                    transform.flattens_inherited_transform,
                );
                if transform.sorting_context_root_index.is_some() || transform.establishes_sorting_context {
                    palette.backface_culled[i] = false;
                }
            }
            SpatialData::Perspective(perspective) => {
                write_spatial(palette, perspective.matrix, perspective.flattens_inherited_transform);
            }
            SpatialData::BackfaceVisibility(backface) => {
                let parent_matrix = palette.to_root_matrices[parent];
                palette.to_root_matrices[i] = if backface.flattens_inherited_transform {
                    parent_matrix.flattened()
                } else {
                    parent_matrix
                };
                palette.local_matrices[i] = FloatMatrix4x4::identity();
                palette.flattens_inherited_transform[i] = backface.flattens_inherited_transform;
                palette.draw_spaces[i] = palette.draw_spaces[parent];
                let mut culled = palette.backface_culled[parent];
                if !culled {
                    let plane_root_matrix = palette.to_root_matrices[backface.plane_root_index.0 as usize];
                    culled = should_cull_back_face(palette.to_root_matrices[i], plane_root_matrix);
                }
                palette.backface_culled[i] = culled;
            }
            SpatialData::Scroll(_) | SpatialData::Sticky(_) => {
                write_spatial_translation(
                    palette,
                    device_offset_for_index(scroll_offsets, SpatialNodeIndex(index)),
                );
            }
            SpatialData::AnchorScrollShift(shift) => {
                write_spatial_translation(palette, shift.masked_offset(scroll_offsets));
            }
            SpatialData::Dead => {}
        }
    }
}

impl<Painter: ReplayPainter> ReplayDriver<'_, Painter> {
    fn build_transform_palette(&mut self, scroll_offsets: &[FloatPoint]) {
        fill_replay_palette_in_dependency_order(
            self.tree,
            &self.spatial_dependency_order,
            scroll_offsets,
            self.replay_base_matrix,
            &mut self.palette,
        );
    }

    fn ensure_ctm_space(&mut self, spatial: SpatialNodeIndex) {
        let space = self.palette.draw_spaces[spatial.0 as usize];
        if self.current_ctm_space == Some(space) {
            return;
        }
        self.painter
            .set_matrix(&self.palette.to_root_matrices[space.0 as usize]);
        self.current_ctm_space = Some(space);
    }

    // Merge the clip and effect chains from leaf to root. Each effect goes immediately
    // inside its output clip; reversing once produces the canvas stack in linear time.
    fn build_target(&mut self, context: ContextRef) {
        let tree = self.tree;
        self.target.clear();
        let mut clip = context.clip;
        let mut effect = context.effect;
        while !effect.is_none() {
            let node = &tree.effect_nodes[effect.0 as usize];
            while clip != self.effect_clips.output_clip(effect) {
                debug_assert!(
                    !clip.is_none(),
                    "an effect's output clip lies on the context's clip chain"
                );
                self.target.push(Applied::Clip(clip));
                clip = tree.clip_nodes[clip.0 as usize].parent;
            }
            self.target.push(Applied::Effect(effect));
            effect = node.parent;
        }
        while !clip.is_none() {
            self.target.push(Applied::Clip(clip));
            clip = tree.clip_nodes[clip.0 as usize].parent;
        }
        self.target.reverse();
    }

    // Contexts in the same effect share every layer. Their clip chains meet at or below
    // that effect's output clip, so only the trailing clips can need to be replaced.
    fn build_target_clip_suffix(&mut self, mut current: ClipNodeIndex, mut target: ClipNodeIndex) -> usize {
        self.target.clear();
        let mut current_depth = self.culling.clip_depth(current);
        let mut target_depth = self.culling.clip_depth(target);
        let mut clips_to_pop = 0;
        while current != target {
            if current_depth >= target_depth {
                current = self.tree.clip_nodes[current.0 as usize].parent;
                current_depth -= 1;
                clips_to_pop += 1;
            } else {
                self.target.push(Applied::Clip(target));
                target = self.tree.clip_nodes[target.0 as usize].parent;
                target_depth -= 1;
            }
        }
        self.target.reverse();
        let common_prefix_length = self.applied.len() - clips_to_pop;
        debug_assert!(
            self.applied[common_prefix_length..]
                .iter()
                .all(|entry| matches!(entry, Applied::Clip(_)))
        );
        common_prefix_length
    }

    fn restore_to_length(&mut self, length: usize) {
        let tree = self.tree;
        self.applied_context = None;
        while self.applied.len() > length {
            match self.applied.pop().expect("applied entries are not empty") {
                Applied::Clip(_) => {
                    self.painter.pop();
                    self.current_ctm_space = None;
                }
                Applied::Effect(effect) => {
                    let node = &tree.effect_nodes[effect.0 as usize];
                    match &node.data {
                        EffectNodeData::Effects(_) => {
                            self.painter.pop();
                            self.current_ctm_space = None;
                        }
                        EffectNodeData::Mask(mask) => {
                            self.ensure_ctm_space(node.spatial);
                            self.painter.pop_mask(&replay_mask_of(mask), effect);
                            self.current_ctm_space = None;
                        }
                        // A marker touches no canvas state.
                        EffectNodeData::BackgroundColorAnimation => {}
                        EffectNodeData::Dead => unreachable!("a run never records under a tombstoned effect"),
                    }
                }
            }
        }
    }

    // OPTIMIZATION: When walking down to layer-pushing effects (opacity/blend/filter layers and masks), check
    //               culling before pushing each one. The clips entered so far are those outside the layer, so
    //               testing against them is conservative and valid. This avoids expensive saveLayer/restore
    //               cycles for off-screen elements.
    fn switch_to_context(&mut self, context: ContextRef, bounding_rect: Option<IntRect>) -> SwitchResult {
        if self.applied_context == Some((context.clip, context.effect)) {
            return SwitchResult::Switched;
        }

        let tree = self.tree;
        let (common_prefix_length, target_begin) = if let Some((clip, effect)) = self.applied_context
            && effect == context.effect
        {
            (self.build_target_clip_suffix(clip, context.clip), 0)
        } else {
            self.build_target(context);
            let common_prefix_length = self
                .applied
                .iter()
                .zip(&self.target)
                .take_while(|(applied, target)| applied == target)
                .count();
            (common_prefix_length, common_prefix_length)
        };

        self.restore_to_length(common_prefix_length);

        for i in target_begin..self.target.len() {
            let entry = self.target[i];
            match entry {
                Applied::Clip(clip) => {
                    let node = &tree.clip_nodes[clip.0 as usize];
                    self.ensure_ctm_space(node.spatial);
                    match &node.data {
                        ClipNodeData::Rect(clip) => self.painter.push_clip(&replay_clip_of(clip)),
                        ClipNodeData::Path(clip_path) => {
                            self.painter.push_clip_path(&clip_path.path, clip_path.fill_rule);
                        }
                        ClipNodeData::Dead => unreachable!("a run never records under a tombstoned clip"),
                    }
                }
                Applied::Effect(effect) => {
                    let node = &tree.effect_nodes[effect.0 as usize];
                    if node.data.pushes_layer()
                        && let Some(bounding_rect) = bounding_rect
                    {
                        let mut culled_by_layer = bounding_rect.is_empty();
                        if !culled_by_layer {
                            self.ensure_ctm_space(context.spatial);
                            culled_by_layer = self.painter.would_be_fully_clipped_by_painter(bounding_rect);
                        }
                        if culled_by_layer {
                            self.restore_to_length(common_prefix_length);
                            // The canvas is unwound to the shared prefix; clearing the applied
                            // context keeps the fast path from reusing the pre-cull context while
                            // the applied stack still enables prefix reuse on the next switch.
                            return SwitchResult::CulledByEffect;
                        }
                    }
                    match &node.data {
                        EffectNodeData::Effects(effects) => {
                            self.ensure_ctm_space(node.spatial);
                            self.painter.push_layer(&replay_layer_of(effects, effect));
                        }
                        EffectNodeData::Mask(mask) => {
                            self.ensure_ctm_space(node.spatial);
                            self.painter.push_mask(&replay_mask_of(mask));
                        }
                        EffectNodeData::BackgroundColorAnimation => {}
                        EffectNodeData::Dead => unreachable!("a run never records under a tombstoned effect"),
                    }
                }
            }
            self.applied.push(entry);
        }

        self.applied_context = Some((context.clip, context.effect));
        SwitchResult::Switched
    }

    // A run enters its context once. Only a run whose ink bounds are known may be skipped as a
    // whole, and only such a run offers its bounds to the layer cull. Skipping a run with
    // nothing to draw before entering its context spares the pushes.
    fn execute_run(&mut self, run_index: usize) {
        let run = self.command_runs[run_index];
        if self.palette.backface_culled[run.context.spatial.0 as usize] {
            return;
        }
        if self.culling.context_culls_everything(run.context) {
            return;
        }
        let skippable_ink_bounds = (!run.has_unbounded_draw).then_some(run.ink_bounds);
        if let Some(bounds) = skippable_ink_bounds
            && bounds.is_empty()
        {
            return;
        }
        if self.switch_to_context(run.context, skippable_ink_bounds) == SwitchResult::CulledByEffect {
            return;
        }
        self.ensure_ctm_space(run.context.spatial);
        if let Some(bounds) = skippable_ink_bounds
            && self.painter.would_be_fully_clipped_by_painter(bounds)
        {
            return;
        }
        self.painter.execute_run(run_index);
    }

    fn execute(&mut self) {
        let tree = self.tree;
        if !self.tree_has_sorting_contexts {
            for run_index in 0..self.command_runs.len() {
                self.execute_run(run_index);
            }
        } else {
            let root_isolation_effect = tree.root_isolation_effect;
            let plane_clip_base_length = usize::from(root_isolation_effect.is_some());
            let contexts = tree.resolve_sorting_contexts_in_order(&self.spatial_dependency_order);
            let parent_by_node: Vec<SpatialNodeIndex> = tree.spatial_nodes.iter().map(|node| node.parent).collect();
            let leaf_to_context_palette = resolve_leaf_to_context_matrices(
                &contexts,
                &self.spatial_dependency_order,
                &parent_by_node,
                &self.palette.local_matrices,
                &self.palette.flattens_inherited_transform,
            );
            let plan = build_depth_sorted_replay_plan(
                self.command_runs,
                &contexts,
                &self.palette.to_root_matrices,
                &leaf_to_context_palette,
                &self.palette.draw_spaces,
                &self.palette.backface_culled,
                &self.culling,
            );
            for step in &plan.steps {
                match step.kind {
                    DepthSortedReplayStepKind::RunSpan => {
                        let first_run = step.first_run as usize;
                        for run_index in first_run..first_run + step.run_count as usize {
                            self.execute_run(run_index);
                        }
                    }
                    DepthSortedReplayStepKind::PushPlaneClip => {
                        // The root effect is a layer at the root of the effect tree under no
                        // clip, so the plane clip sits right above it.
                        if let Some(root_isolation_effect) = root_isolation_effect {
                            self.switch_to_context(
                                ContextRef {
                                    spatial: tree.effect_nodes[root_isolation_effect.0 as usize].spatial,
                                    clip: ClipNodeIndex::NONE,
                                    effect: root_isolation_effect,
                                },
                                None,
                            );
                        }
                        self.restore_to_length(plane_clip_base_length);
                        let vertex_offset = step.vertex_offset as usize;
                        self.painter.push_device_space_plane_clip(
                            &plan.vertices[vertex_offset..vertex_offset + step.vertex_count as usize],
                        );
                        self.current_ctm_space = None;
                    }
                    DepthSortedReplayStepKind::PopPlaneClip => {
                        self.restore_to_length(plane_clip_base_length);
                        self.painter.pop();
                        self.current_ctm_space = None;
                    }
                }
            }
        }

        self.restore_to_length(0);
        // Node spaces were entered by setting the canvas matrix absolutely, outside any save, so the
        // matrix the replay entered with must be handed back explicitly.
        let replay_base_matrix = self.replay_base_matrix;
        self.painter.set_matrix(&replay_base_matrix);
    }
}

pub fn replay_display_list(
    tree: &VisualContextTree,
    command_runs: &[DisplayListCommandRun],
    effect_clips: &EffectClipPlan,
    scroll_offsets: &[FloatPoint],
    painter: &mut impl ReplayPainter,
) {
    let replay_base_matrix = painter.canvas_matrix();
    let mut scratch = take_replay_scratch_storage();
    tree.fill_culling_scratch(&mut scratch.culling);
    let mut driver = ReplayDriver {
        tree,
        command_runs,
        effect_clips,
        painter,
        palette: scratch.palette,
        culling: scratch.culling,
        spatial_dependency_order: tree.spatial_dependency_order(),
        tree_has_sorting_contexts: tree.spatial_nodes.iter().any(|node| {
            matches!(&node.data, SpatialData::Transform(transform) if transform.sorting_context_root_index.is_some())
        }),
        replay_base_matrix,
        current_ctm_space: None,
        applied: scratch.applied,
        target: scratch.target,
        applied_context: None,
    };
    driver.build_transform_palette(scroll_offsets);
    driver.execute();
    return_replay_scratch_storage(ReplayScratchStorage {
        palette: std::mem::take(&mut driver.palette),
        culling: std::mem::take(&mut driver.culling),
        applied: std::mem::take(&mut driver.applied),
        target: std::mem::take(&mut driver.target),
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::node_data::NodeSlotId;
    use crate::painting::display_list::commands::ContextRef;
    use crate::painting::display_list::commands::VISUAL_VIEWPORT_NODE_INDEX;
    use crate::painting::visual_context::{
        BackdropFilterData, BackfaceVisibilityData, ClipData, ClipMode, EffectsData, MaskData, MaskLayerOrigin,
        SpatialData, TransformData, TransformDataRole,
    };
    use libgfx_rust::{
        CompositingAndBlendingOperator, CornerRadii, FloatRect, MaskKind, scale_matrix, translation_matrix,
    };

    #[test]
    fn a_child_stored_below_its_parent_gets_the_same_palette_entry() {
        let root = TransformData {
            matrix: FloatMatrix4x4::identity(),
            origin: FloatPoint::default(),
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
        };
        let translated = |x: f32, y: f32| {
            SpatialData::Transform(TransformData {
                matrix: translation_matrix(x, y, 0.0),
                ..root
            })
        };
        let flipped = SpatialData::Transform(TransformData {
            matrix: scale_matrix(-1.0, 1.0, -1.0),
            ..root
        });

        let mut in_order = VisualContextTree::create(root);
        let parent = in_order.append_spatial(translated(10.0, 0.0), VISUAL_VIEWPORT_NODE_INDEX);
        let child = in_order.append_spatial(translated(0.0, 5.0), parent);
        let plane = in_order.append_spatial(flipped.clone(), child);
        let marker = in_order.append_spatial(
            SpatialData::BackfaceVisibility(BackfaceVisibilityData {
                plane_root_index: child,
                flattens_inherited_transform: false,
            }),
            plane,
        );

        let mut permuted = VisualContextTree::create(root);
        let permuted_marker = permuted.append_spatial(
            SpatialData::BackfaceVisibility(BackfaceVisibilityData {
                plane_root_index: SpatialNodeIndex(3),
                flattens_inherited_transform: false,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let permuted_plane = permuted.append_spatial(flipped, VISUAL_VIEWPORT_NODE_INDEX);
        let permuted_child = permuted.append_spatial(translated(0.0, 5.0), VISUAL_VIEWPORT_NODE_INDEX);
        let permuted_parent = permuted.append_spatial(translated(10.0, 0.0), VISUAL_VIEWPORT_NODE_INDEX);
        permuted.spatial_nodes[permuted_marker.0 as usize].parent = permuted_plane;
        permuted.spatial_nodes[permuted_plane.0 as usize].parent = permuted_child;
        permuted.spatial_nodes[permuted_child.0 as usize].parent = permuted_parent;

        let base = scale_matrix(2.0, 2.0, 1.0);
        let mut in_order_palette = ReplayPaletteStorage::default();
        fill_replay_palette_in_dependency_order(
            &in_order,
            &in_order.spatial_dependency_order(),
            &[],
            base,
            &mut in_order_palette,
        );
        let mut permuted_palette = ReplayPaletteStorage::default();
        fill_replay_palette_in_dependency_order(
            &permuted,
            &permuted.spatial_dependency_order(),
            &[],
            base,
            &mut permuted_palette,
        );
        let pairs = [
            (VISUAL_VIEWPORT_NODE_INDEX, VISUAL_VIEWPORT_NODE_INDEX),
            (parent, permuted_parent),
            (child, permuted_child),
            (plane, permuted_plane),
            (marker, permuted_marker),
        ];
        let map = |index: SpatialNodeIndex| pairs.iter().find(|(original, _)| *original == index).unwrap().1;
        for (original, mapped) in pairs {
            assert_eq!(
                permuted_palette.to_root_matrices[mapped.0 as usize],
                in_order_palette.to_root_matrices[original.0 as usize]
            );
            assert_eq!(
                permuted_palette.local_matrices[mapped.0 as usize],
                in_order_palette.local_matrices[original.0 as usize]
            );
            assert_eq!(
                permuted_palette.draw_spaces[mapped.0 as usize],
                map(in_order_palette.draw_spaces[original.0 as usize])
            );
            assert_eq!(
                permuted_palette.backface_culled[mapped.0 as usize],
                in_order_palette.backface_culled[original.0 as usize]
            );
            assert_eq!(
                permuted_palette.flattens_inherited_transform[mapped.0 as usize],
                in_order_palette.flattens_inherited_transform[original.0 as usize]
            );
        }
        assert!(in_order_palette.backface_culled[marker.0 as usize]);
        assert_eq!(in_order_palette.draw_spaces[marker.0 as usize], plane);
    }

    #[derive(Debug, PartialEq)]
    enum PainterEvent {
        SetMatrix(FloatMatrix4x4),
        PushClip(FloatRect),
        PushLayer(f32),
        PushMask(IntRect),
        PopMask(EffectNodeIndex),
        Pop,
        PushPlaneClip(usize),
        Run(usize),
    }

    struct RecordingPainter {
        events: Vec<PainterEvent>,
        base_matrix: FloatMatrix4x4,
        fully_clipped_x: i32,
    }

    impl RecordingPainter {
        fn new() -> Self {
            Self {
                events: Vec::new(),
                base_matrix: translation_matrix(100.0, 200.0, 0.0),
                fully_clipped_x: i32::MIN,
            }
        }
    }

    impl ReplayPainter for RecordingPainter {
        fn canvas_matrix(&mut self) -> FloatMatrix4x4 {
            self.base_matrix
        }
        fn set_matrix(&mut self, matrix: &FloatMatrix4x4) {
            self.events.push(PainterEvent::SetMatrix(*matrix));
        }
        fn would_be_fully_clipped_by_painter(&mut self, rect: IntRect) -> bool {
            rect.x == self.fully_clipped_x
        }
        fn push_clip(&mut self, clip: &ReplayClip) {
            self.events.push(PainterEvent::PushClip(clip.rect));
        }
        fn push_clip_path(&mut self, _path: &OwnedPath, _winding_rule: WindingRule) {
            unreachable!("these tests build no clip paths");
        }
        fn push_layer(&mut self, layer: &ReplayLayer) {
            self.events.push(PainterEvent::PushLayer(layer.opacity));
        }
        fn push_mask(&mut self, mask: &ReplayMask) {
            self.events.push(PainterEvent::PushMask(mask.rect));
        }
        fn pop_mask(&mut self, _mask: &ReplayMask, effect: EffectNodeIndex) {
            self.events.push(PainterEvent::PopMask(effect));
        }
        fn pop(&mut self) {
            self.events.push(PainterEvent::Pop);
        }
        fn push_device_space_plane_clip(&mut self, vertices: &[FloatVector3]) {
            self.events.push(PainterEvent::PushPlaneClip(vertices.len()));
        }
        fn execute_run(&mut self, run_index: usize) {
            self.events.push(PainterEvent::Run(run_index));
        }
    }

    fn transform(matrix: FloatMatrix4x4, sorting_context_root_index: Option<SpatialNodeIndex>) -> SpatialData {
        SpatialData::Transform(TransformData {
            matrix,
            origin: FloatPoint::default(),
            sorting_context_root_index,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
        })
    }

    fn identity_tree() -> VisualContextTree {
        let SpatialData::Transform(root) = transform(FloatMatrix4x4::identity(), None) else {
            unreachable!()
        };
        VisualContextTree::create(root)
    }

    fn clip(rect: FloatRect) -> ClipNodeData {
        ClipNodeData::Rect(ClipData {
            rect,
            corner_radii: CornerRadii::default(),
            mode: ClipMode::Intersect,
        })
    }

    fn effects(opacity: f32) -> EffectNodeData {
        EffectNodeData::Effects(EffectsData {
            opacity,
            blend_mode: CompositingAndBlendingOperator::Normal,
            filter: None,
            backdrop_filter: None,
        })
    }

    fn mask(rect: IntRect) -> EffectNodeData {
        EffectNodeData::Mask(MaskData {
            rect,
            kind: MaskKind::Alpha,
            origin: MaskLayerOrigin::CssMaskLayers,
        })
    }

    fn root_clip(tree: &mut VisualContextTree, rect: FloatRect) -> ClipNodeIndex {
        tree.append_clip(clip(rect), ClipNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX)
    }

    fn effect_under(
        tree: &mut VisualContextTree,
        data: EffectNodeData,
        parent: EffectNodeIndex,
        output_clip: ClipNodeIndex,
    ) -> EffectNodeIndex {
        tree.append_effect(data, parent, VISUAL_VIEWPORT_NODE_INDEX, output_clip)
    }

    fn run(spatial: SpatialNodeIndex, context: ContextRef, ink_bounds: IntRect) -> DisplayListCommandRun {
        DisplayListCommandRun {
            offset: 0,
            size: 0,
            context: ContextRef { spatial, ..context },
            ink_bounds,
            has_unbounded_draw: false,
            has_compositor_metadata: false,
        }
    }

    fn rotate_y(degrees: f32) -> FloatMatrix4x4 {
        let (sin, cos) = degrees.to_radians().sin_cos();
        FloatMatrix4x4 {
            elements: [
                [cos, 0.0, sin, 0.0],
                [0.0, 1.0, 0.0, 0.0],
                [-sin, 0.0, cos, 0.0],
                [0.0, 0.0, 0.0, 1.0],
            ],
        }
    }

    fn visible_bounds() -> IntRect {
        IntRect::new(0, 0, 10, 10)
    }

    fn replay(tree: &VisualContextTree, runs: &[DisplayListCommandRun], painter: &mut RecordingPainter) {
        replay_display_list(tree, runs, &EffectClipPlan::new(tree, runs).unwrap(), &[], painter);
    }

    #[test]
    fn runs_sharing_a_stack_prefix_push_it_once_and_re_enter_their_space_after_a_pop() {
        let mut tree = identity_tree();
        let clip_node = root_clip(&mut tree, FloatRect::new(1.0, 2.0, 3.0, 4.0));
        let clip_context = ContextRef {
            clip: clip_node,
            effect: EffectNodeIndex::NONE,
            ..ContextRef::default()
        };
        let layer = effect_under(&mut tree, effects(0.5), EffectNodeIndex::NONE, clip_node);
        let layer_context = ContextRef {
            clip: clip_node,
            effect: layer,
            ..ContextRef::default()
        };
        let runs = [
            run(VISUAL_VIEWPORT_NODE_INDEX, clip_context, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, layer_context, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, clip_context, visible_bounds()),
        ];
        let mut painter = RecordingPainter::new();
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::SetMatrix(base),
                PainterEvent::PushClip(FloatRect::new(1.0, 2.0, 3.0, 4.0)),
                PainterEvent::Run(0),
                PainterEvent::PushLayer(0.5),
                PainterEvent::Run(1),
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
                PainterEvent::Run(2),
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
            ]
        );
    }

    #[test]
    fn a_run_under_an_empty_effective_clip_touches_nothing() {
        let mut tree = identity_tree();
        let empty_clip = root_clip(&mut tree, FloatRect::default());
        let empty_context = ContextRef {
            clip: empty_clip,
            effect: EffectNodeIndex::NONE,
            ..ContextRef::default()
        };
        let empty_mask = effect_under(
            &mut tree,
            mask(IntRect::default()),
            EffectNodeIndex::NONE,
            ClipNodeIndex::NONE,
        );
        let empty_mask_context = ContextRef {
            clip: ClipNodeIndex::NONE,
            effect: empty_mask,
            ..ContextRef::default()
        };
        let runs = [
            run(VISUAL_VIEWPORT_NODE_INDEX, empty_context, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, empty_mask_context, visible_bounds()),
        ];
        let mut painter = RecordingPainter::new();
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        assert_eq!(painter.events, vec![PainterEvent::SetMatrix(base)]);
    }

    #[test]
    fn a_fully_clipped_layer_is_culled_and_the_next_run_pushes_the_stack_again() {
        let mut tree = identity_tree();
        let clip_node = root_clip(&mut tree, FloatRect::new(0.0, 0.0, 50.0, 50.0));
        let layer = effect_under(&mut tree, effects(0.25), EffectNodeIndex::NONE, clip_node);
        let layer_context = ContextRef {
            clip: clip_node,
            effect: layer,
            ..ContextRef::default()
        };
        let off_screen = IntRect::new(999, 0, 10, 10);
        let runs = [
            run(VISUAL_VIEWPORT_NODE_INDEX, layer_context, off_screen),
            run(VISUAL_VIEWPORT_NODE_INDEX, layer_context, visible_bounds()),
        ];
        let mut painter = RecordingPainter::new();
        painter.fully_clipped_x = 999;
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::SetMatrix(base),
                PainterEvent::PushClip(FloatRect::new(0.0, 0.0, 50.0, 50.0)),
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
                PainterEvent::PushClip(FloatRect::new(0.0, 0.0, 50.0, 50.0)),
                PainterEvent::PushLayer(0.25),
                PainterEvent::Run(1),
                PainterEvent::Pop,
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
            ]
        );
    }

    #[test]
    fn mask_effects_pop_with_their_effect_index() {
        let mut tree = identity_tree();
        let mask_effect = effect_under(
            &mut tree,
            mask(IntRect::new(0, 0, 20, 20)),
            EffectNodeIndex::NONE,
            ClipNodeIndex::NONE,
        );
        let mask_context = ContextRef {
            clip: ClipNodeIndex::NONE,
            effect: mask_effect,
            ..ContextRef::default()
        };
        let runs = [run(VISUAL_VIEWPORT_NODE_INDEX, mask_context, visible_bounds())];
        let mut painter = RecordingPainter::new();
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::SetMatrix(base),
                PainterEvent::PushMask(IntRect::new(0, 0, 20, 20)),
                PainterEvent::Run(0),
                PainterEvent::PopMask(mask_effect),
                PainterEvent::SetMatrix(base),
            ]
        );
    }

    // An absolute child that skips its parent's overflow clip records under the parent's effect
    // and the effect's output clip: the layer is entered once and the overflow clip moves in and
    // out of it around the child's run.
    #[test]
    fn an_escaping_absolute_child_under_an_opacity_layer_pushes_the_layer_once() {
        let mut tree = identity_tree();
        let layer = effect_under(&mut tree, effects(0.5), EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        let overflow_clip = root_clip(&mut tree, FloatRect::new(0.0, 0.0, 100.0, 100.0));
        let normal_context = ContextRef {
            clip: overflow_clip,
            effect: layer,
            ..ContextRef::default()
        };
        let escaping_context = ContextRef {
            clip: ClipNodeIndex::NONE,
            effect: layer,
            ..ContextRef::default()
        };
        let runs = [
            run(VISUAL_VIEWPORT_NODE_INDEX, normal_context, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, escaping_context, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, normal_context, visible_bounds()),
        ];
        let mut painter = RecordingPainter::new();
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::SetMatrix(base),
                PainterEvent::PushLayer(0.5),
                PainterEvent::PushClip(FloatRect::new(0.0, 0.0, 100.0, 100.0)),
                PainterEvent::Run(0),
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
                PainterEvent::Run(1),
                PainterEvent::PushClip(FloatRect::new(0.0, 0.0, 100.0, 100.0)),
                PainterEvent::Run(2),
                PainterEvent::Pop,
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
            ]
        );
    }

    // A box appends an effect with the widest clip a chain could take; tightening moves it back
    // down to the clips its contexts share, so the layer is entered inside them.
    #[test]
    fn a_layer_nothing_escapes_is_entered_inside_the_clips_its_contexts_share() {
        let mut tree = identity_tree();
        let layer = effect_under(&mut tree, effects(0.5), EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        let outer_clip = root_clip(&mut tree, FloatRect::new(0.0, 0.0, 100.0, 100.0));
        let inner_clip = tree.append_clip(
            clip(FloatRect::new(0.0, 0.0, 50.0, 50.0)),
            outer_clip,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let nested = effect_under(&mut tree, effects(0.25), layer, ClipNodeIndex::NONE);
        let outer_context = ContextRef {
            clip: outer_clip,
            effect: layer,
            ..ContextRef::default()
        };
        let nested_context = ContextRef {
            clip: inner_clip,
            effect: nested,
            ..ContextRef::default()
        };
        tree.effect_nodes[layer.0 as usize].local_clip = outer_clip;
        tree.effect_nodes[nested.0 as usize].local_clip = inner_clip;
        let runs = [
            run(VISUAL_VIEWPORT_NODE_INDEX, outer_context, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, nested_context, visible_bounds()),
        ];
        let mut painter = RecordingPainter::new();
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::SetMatrix(base),
                PainterEvent::PushClip(FloatRect::new(0.0, 0.0, 100.0, 100.0)),
                PainterEvent::PushLayer(0.5),
                PainterEvent::Run(0),
                PainterEvent::PushClip(FloatRect::new(0.0, 0.0, 50.0, 50.0)),
                PainterEvent::PushLayer(0.25),
                PainterEvent::Run(1),
                PainterEvent::Pop,
                PainterEvent::Pop,
                PainterEvent::Pop,
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
            ]
        );
    }

    // Two chains that differ only in their clip copies swap clips inside the shared layer.
    #[test]
    fn a_clip_inside_a_layer_is_re_pushed_when_switching_chains_without_popping_the_layer() {
        let mut tree = identity_tree();
        let layer = effect_under(&mut tree, effects(0.5), EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        let own_clip = root_clip(&mut tree, FloatRect::new(0.0, 0.0, 10.0, 10.0));
        let escaping_clip = root_clip(&mut tree, FloatRect::new(0.0, 0.0, 20.0, 20.0));
        let own_context = ContextRef {
            clip: own_clip,
            effect: layer,
            ..ContextRef::default()
        };
        let escaping_context = ContextRef {
            clip: escaping_clip,
            effect: layer,
            ..ContextRef::default()
        };
        let runs = [
            run(VISUAL_VIEWPORT_NODE_INDEX, own_context, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, escaping_context, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, own_context, visible_bounds()),
        ];
        let mut painter = RecordingPainter::new();
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::SetMatrix(base),
                PainterEvent::PushLayer(0.5),
                PainterEvent::PushClip(FloatRect::new(0.0, 0.0, 10.0, 10.0)),
                PainterEvent::Run(0),
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
                PainterEvent::PushClip(FloatRect::new(0.0, 0.0, 20.0, 20.0)),
                PainterEvent::Run(1),
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
                PainterEvent::PushClip(FloatRect::new(0.0, 0.0, 10.0, 10.0)),
                PainterEvent::Run(2),
                PainterEvent::Pop,
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
            ]
        );
    }

    #[test]
    fn switching_clip_depths_inside_one_effect_keeps_the_layer_and_its_output_clip() {
        let mut tree = identity_tree();
        let outer_rect = FloatRect::new(0.0, 0.0, 100.0, 100.0);
        let common_rect = FloatRect::new(0.0, 0.0, 90.0, 90.0);
        let deep_rect = FloatRect::new(0.0, 0.0, 80.0, 80.0);
        let other_rect = FloatRect::new(0.0, 0.0, 70.0, 70.0);
        let outer = root_clip(&mut tree, outer_rect);
        let common = tree.append_clip(clip(common_rect), outer, VISUAL_VIEWPORT_NODE_INDEX);
        let deep = tree.append_clip(clip(deep_rect), common, VISUAL_VIEWPORT_NODE_INDEX);
        let other = tree.append_clip(clip(other_rect), outer, VISUAL_VIEWPORT_NODE_INDEX);
        let effect = effect_under(&mut tree, effects(0.5), EffectNodeIndex::NONE, outer);
        let runs = [deep, other, outer, common].map(|clip| {
            run(
                VISUAL_VIEWPORT_NODE_INDEX,
                ContextRef {
                    clip,
                    effect,
                    ..ContextRef::default()
                },
                visible_bounds(),
            )
        });
        let mut painter = RecordingPainter::new();
        replay(&tree, &runs, &mut painter);
        let events: Vec<_> = painter
            .events
            .into_iter()
            .filter(|event| !matches!(event, PainterEvent::SetMatrix(_)))
            .collect();
        assert_eq!(
            events,
            vec![
                PainterEvent::PushClip(outer_rect),
                PainterEvent::PushLayer(0.5),
                PainterEvent::PushClip(common_rect),
                PainterEvent::PushClip(deep_rect),
                PainterEvent::Run(0),
                PainterEvent::Pop,
                PainterEvent::Pop,
                PainterEvent::PushClip(other_rect),
                PainterEvent::Run(1),
                PainterEvent::Pop,
                PainterEvent::Run(2),
                PainterEvent::PushClip(common_rect),
                PainterEvent::Run(3),
                PainterEvent::Pop,
                PainterEvent::Pop,
                PainterEvent::Pop,
            ]
        );
    }

    #[test]
    fn a_later_clip_escape_keeps_the_opacity_group_open() {
        let mut tree = identity_tree();
        let rect = FloatRect::new(0.0, 0.0, 100.0, 100.0);
        let local_clip = root_clip(&mut tree, rect);
        let effect = effect_under(&mut tree, effects(0.5), EffectNodeIndex::NONE, local_clip);
        let runs = [local_clip, ClipNodeIndex::NONE].map(|clip| {
            run(
                VISUAL_VIEWPORT_NODE_INDEX,
                ContextRef {
                    clip,
                    effect,
                    ..ContextRef::default()
                },
                visible_bounds(),
            )
        });
        let mut painter = RecordingPainter::new();
        replay(&tree, &runs, &mut painter);
        let events: Vec<_> = painter
            .events
            .into_iter()
            .filter(|event| !matches!(event, PainterEvent::SetMatrix(_)))
            .collect();
        assert_eq!(
            events,
            vec![
                PainterEvent::PushLayer(0.5),
                PainterEvent::PushClip(rect),
                PainterEvent::Run(0),
                PainterEvent::Pop,
                PainterEvent::Run(1),
                PainterEvent::Pop,
            ]
        );
    }

    // A mask nested in a layer is entered inside its own output clip, which may sit above a clip
    // pushed inside the layer; that clip is re-pushed inside the mask and the mask exits once.
    #[test]
    fn a_mask_nested_in_a_layer_pops_once_with_its_effect_index() {
        let mut tree = identity_tree();
        let layer = effect_under(&mut tree, effects(0.5), EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        let overflow_clip = root_clip(&mut tree, FloatRect::new(0.0, 0.0, 100.0, 100.0));
        let mask_effect = effect_under(&mut tree, mask(IntRect::new(0, 0, 20, 20)), layer, ClipNodeIndex::NONE);
        let layer_context = ContextRef {
            clip: overflow_clip,
            effect: layer,
            ..ContextRef::default()
        };
        let masked_context = ContextRef {
            clip: overflow_clip,
            effect: mask_effect,
            ..ContextRef::default()
        };
        let escaping_masked_context = ContextRef {
            clip: ClipNodeIndex::NONE,
            effect: mask_effect,
            ..ContextRef::default()
        };
        let runs = [
            run(VISUAL_VIEWPORT_NODE_INDEX, layer_context, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, masked_context, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, escaping_masked_context, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, layer_context, visible_bounds()),
        ];
        let mut painter = RecordingPainter::new();
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        let overflow = FloatRect::new(0.0, 0.0, 100.0, 100.0);
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::SetMatrix(base),
                PainterEvent::PushLayer(0.5),
                PainterEvent::PushClip(overflow),
                PainterEvent::Run(0),
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
                PainterEvent::PushMask(IntRect::new(0, 0, 20, 20)),
                PainterEvent::PushClip(overflow),
                PainterEvent::Run(1),
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
                PainterEvent::Run(2),
                PainterEvent::PopMask(mask_effect),
                PainterEvent::SetMatrix(base),
                PainterEvent::PushClip(overflow),
                PainterEvent::Run(3),
                PainterEvent::Pop,
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
            ]
        );
    }

    #[test]
    fn a_layer_carries_its_backdrop_filter() {
        let filter = std::rc::Rc::new(vec![1, 2, 3]);
        let region = IntRect::new(1, 2, 30, 40);
        let radii = CornerRadii::uniform(5);
        let with_backdrop = EffectsData {
            opacity: 0.5,
            blend_mode: CompositingAndBlendingOperator::Normal,
            filter: None,
            backdrop_filter: Some(BackdropFilterData {
                filter: filter.clone(),
                region,
                corner_radii: radii,
            }),
        };
        let layer = replay_layer_of(&with_backdrop, EffectNodeIndex(3));
        assert_eq!(layer.opacity, 0.5);
        assert_eq!(layer.backdrop_filter_bytes, filter.as_ptr());
        assert_eq!(layer.backdrop_filter_bytes_size, 3);
        assert_eq!(layer.backdrop_region, region);
        assert_eq!(layer.backdrop_corner_radii, radii);
        assert_eq!(layer.effect, EffectNodeIndex(3));

        let EffectNodeData::Effects(without_backdrop) = effects(0.5) else {
            unreachable!()
        };
        let layer = replay_layer_of(&without_backdrop, EffectNodeIndex(4));
        assert!(layer.backdrop_filter_bytes.is_null());
        assert_eq!(layer.backdrop_filter_bytes_size, 0);
        assert_eq!(layer.backdrop_region, IntRect::default());
    }

    // A background-color animation marker is an effect without canvas state: entering or leaving
    // it neither pushes nor invalidates the matrix.
    #[test]
    fn a_background_color_animation_marker_pushes_nothing() {
        let mut tree = identity_tree();
        let layer = effect_under(&mut tree, effects(0.5), EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        let marker = effect_under(
            &mut tree,
            EffectNodeData::BackgroundColorAnimation,
            layer,
            ClipNodeIndex::NONE,
        );
        let layer_context = ContextRef {
            clip: ClipNodeIndex::NONE,
            effect: layer,
            ..ContextRef::default()
        };
        let marker_context = ContextRef {
            clip: ClipNodeIndex::NONE,
            effect: marker,
            ..ContextRef::default()
        };
        let runs = [
            run(VISUAL_VIEWPORT_NODE_INDEX, marker_context, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, layer_context, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, marker_context, visible_bounds()),
        ];
        let mut painter = RecordingPainter::new();
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::SetMatrix(base),
                PainterEvent::PushLayer(0.5),
                PainterEvent::Run(0),
                PainterEvent::Run(1),
                PainterEvent::Run(2),
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
            ]
        );
    }

    // Culling the second of two new layers unwinds to the shared prefix, so the next run rebuilds
    // from there.
    #[test]
    fn culling_the_second_of_two_new_layers_unwinds_to_the_shared_prefix() {
        let mut tree = identity_tree();
        let clip_node = root_clip(&mut tree, FloatRect::new(0.0, 0.0, 50.0, 50.0));
        let outer = effect_under(&mut tree, effects(0.5), EffectNodeIndex::NONE, clip_node);
        let inner = effect_under(&mut tree, effects(0.25), outer, clip_node);
        let inner_context = ContextRef {
            clip: clip_node,
            effect: inner,
            ..ContextRef::default()
        };
        let off_screen = IntRect::new(999, 0, 10, 10);
        let runs = [
            run(VISUAL_VIEWPORT_NODE_INDEX, inner_context, off_screen),
            run(VISUAL_VIEWPORT_NODE_INDEX, inner_context, visible_bounds()),
        ];
        let mut painter = RecordingPainter::new();
        painter.fully_clipped_x = 999;
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::SetMatrix(base),
                PainterEvent::PushClip(FloatRect::new(0.0, 0.0, 50.0, 50.0)),
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
                PainterEvent::PushClip(FloatRect::new(0.0, 0.0, 50.0, 50.0)),
                PainterEvent::PushLayer(0.5),
                PainterEvent::PushLayer(0.25),
                PainterEvent::Run(1),
                PainterEvent::Pop,
                PainterEvent::Pop,
                PainterEvent::Pop,
                PainterEvent::SetMatrix(base),
            ]
        );
    }

    #[test]
    fn runs_on_a_culled_back_face_are_skipped_and_scroll_offsets_enter_the_palette() {
        let mut tree = identity_tree();
        let flipped = tree.append_spatial(
            transform(scale_matrix(-1.0, 1.0, -1.0), None),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let marker = tree.append_spatial(
            SpatialData::BackfaceVisibility(BackfaceVisibilityData {
                plane_root_index: VISUAL_VIEWPORT_NODE_INDEX,
                flattens_inherited_transform: false,
            }),
            flipped,
        );
        let scroll_node = tree.append_spatial(
            SpatialData::Scroll(crate::painting::visual_context::ScrollData {
                state_slot: crate::painting::visual_context::scroll_state::NO_SCROLL_STATE_SLOT,
                owner_paintable: NodeSlotId::INVALID,
                registry_parent_node: VISUAL_VIEWPORT_NODE_INDEX,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let runs = [
            run(marker, ContextRef::default(), visible_bounds()),
            run(scroll_node, ContextRef::default(), visible_bounds()),
        ];
        let mut scroll_offsets = vec![FloatPoint::default(); 4];
        scroll_offsets[scroll_node.0 as usize] = FloatPoint { x: 0.0, y: -30.7 };
        let mut painter = RecordingPainter::new();
        let base = painter.base_matrix;
        replay_display_list(
            &tree,
            &runs,
            &EffectClipPlan::new(&tree, &runs).unwrap(),
            &scroll_offsets,
            &mut painter,
        );
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::SetMatrix(base.multiplied(translation_matrix(0.0, -30.0, 0.0))),
                PainterEvent::Run(1),
                PainterEvent::SetMatrix(base),
            ]
        );
    }

    #[test]
    fn intersecting_planes_of_a_sorting_context_replay_under_plane_clips() {
        let mut tree = identity_tree();
        let context_root = tree.append_spatial(transform(FloatMatrix4x4::identity(), None), VISUAL_VIEWPORT_NODE_INDEX);
        let first_plane = tree.append_spatial(transform(rotate_y(45.0), Some(context_root)), context_root);
        let second_plane = tree.append_spatial(transform(rotate_y(-45.0), Some(context_root)), context_root);
        let plane_bounds = IntRect::new(-50, -50, 100, 100);
        let runs = [
            run(first_plane, ContextRef::default(), plane_bounds),
            run(second_plane, ContextRef::default(), plane_bounds),
        ];
        let mut painter = RecordingPainter::new();
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        let plane_clip_pushes = painter
            .events
            .iter()
            .filter(|event| matches!(event, PainterEvent::PushPlaneClip(_)))
            .count();
        let pops = painter
            .events
            .iter()
            .filter(|event| **event == PainterEvent::Pop)
            .count();
        let executed_runs = painter
            .events
            .iter()
            .filter(|event| matches!(event, PainterEvent::Run(_)))
            .count();
        assert!(plane_clip_pushes > 0);
        assert_eq!(pops, plane_clip_pushes);
        assert!(executed_runs >= runs.len());
        assert_eq!(painter.events.last(), Some(&PainterEvent::SetMatrix(base)));
    }

    // Plane clips are pushed right above the root isolation layer: everything under it unwinds
    // around each plane boundary, and the layer itself is entered once.
    #[test]
    fn plane_clips_unwind_to_the_root_isolation_layer() {
        let mut tree = identity_tree();
        let root_layer = effect_under(&mut tree, effects(1.0), EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        tree.root_isolation_effect = Some(root_layer);
        let clip_node = root_clip(&mut tree, FloatRect::new(-100.0, -100.0, 300.0, 300.0));
        let clipped_context = ContextRef {
            clip: clip_node,
            effect: root_layer,
            ..ContextRef::default()
        };
        let context_root = tree.append_spatial(transform(FloatMatrix4x4::identity(), None), VISUAL_VIEWPORT_NODE_INDEX);
        let first_plane = tree.append_spatial(transform(rotate_y(45.0), Some(context_root)), context_root);
        let second_plane = tree.append_spatial(transform(rotate_y(-45.0), Some(context_root)), context_root);
        let plane_bounds = IntRect::new(-50, -50, 100, 100);
        let runs = [
            run(first_plane, clipped_context, plane_bounds),
            run(second_plane, clipped_context, plane_bounds),
        ];
        let mut painter = RecordingPainter::new();
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        let layer_pushes = painter
            .events
            .iter()
            .filter(|event| matches!(event, PainterEvent::PushLayer(_)))
            .count();
        assert_eq!(layer_pushes, 1);
        let mut depth = 0i32;
        let mut plane_clip_pushes = 0;
        for event in &painter.events {
            match event {
                PainterEvent::PushPlaneClip(_) => {
                    assert_eq!(depth, 1, "a plane clip is pushed right above the root layer");
                    plane_clip_pushes += 1;
                    depth += 1;
                }
                PainterEvent::PushClip(_) | PainterEvent::PushLayer(_) | PainterEvent::PushMask(_) => depth += 1,
                PainterEvent::Pop | PainterEvent::PopMask(_) => depth -= 1,
                _ => {}
            }
        }
        assert!(plane_clip_pushes > 0);
        assert_eq!(depth, 0);
        assert_eq!(painter.events.last(), Some(&PainterEvent::SetMatrix(base)));
    }
}
