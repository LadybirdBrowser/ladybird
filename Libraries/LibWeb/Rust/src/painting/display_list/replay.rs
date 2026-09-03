/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::display_list::commands::{
    ContextRef, DisplayListCommandRun, FrameNodeIndex, ReplayClip, ReplayLayer, ReplayMask, SpatialNodeIndex,
};
use crate::painting::display_list::depth_sorted_plan::{DepthSortedReplayStepKind, build_depth_sorted_replay_plan};
use crate::painting::visual_context::{
    FrameData, SpatialData, VisualContextTree, device_offset_for_index, resolve_leaf_to_context_matrices,
    should_cull_back_face,
};
use libgfx_rust::path::OwnedPath;
use libgfx_rust::{FloatMatrix4x4, FloatPoint, FloatVector3, IntRect, WindingRule, translation_matrix};
use std::cell::RefCell;

pub trait ReplayPainter {
    fn canvas_matrix(&mut self) -> FloatMatrix4x4;
    fn set_matrix(&mut self, matrix: &FloatMatrix4x4);
    fn would_be_fully_clipped_by_painter(&mut self, rect: IntRect) -> bool;
    fn push_clip(&mut self, clip: &ReplayClip);
    fn push_clip_path(&mut self, path: &OwnedPath, winding_rule: WindingRule);
    fn push_layer(&mut self, layer: &ReplayLayer);
    fn push_mask(&mut self, mask: &ReplayMask);
    fn pop_mask(&mut self, mask: &ReplayMask, frame: FrameNodeIndex);
    fn pop(&mut self);
    fn push_device_space_plane_clip(&mut self, vertices: &[FloatVector3]);
    fn execute_run(&mut self, run_index: usize);
}

// Cumulative to-root matrices for every spatial node, resolved against the live scroll offsets
// and folded onto the canvas matrix at replay entry, so any node's space can be entered
// absolutely with a single set_matrix(). Spatial nodes therefore never touch the canvas save
// stack; only frames do. A backface marker's entry carries the flattened matrix that feeds its
// cull test and its descendants, while content recorded directly under the marker belongs to
// its parent's plane, so draw_space redirects the marker to the parent's entry.
#[derive(Default)]
struct ReplayPaletteStorage {
    to_root_matrices: Vec<FloatMatrix4x4>,
    local_matrices: Vec<FloatMatrix4x4>,
    draw_spaces: Vec<SpatialNodeIndex>,
    backface_culled: Vec<bool>,
    flattens_inherited_transform: Vec<bool>,
}

// Steady-state replays reuse the previous frame's capacity. Taking the storage out of the slot
// for the duration of a replay keeps re-entrant nested replays from clobbering the outer
// driver's buffers; the slot keeps whichever returned storage has the larger palette.
#[derive(Default)]
struct ReplayScratchStorage {
    palette: ReplayPaletteStorage,
    frame_has_empty_effective_clip: Vec<bool>,
    applied_frames: Vec<FrameNodeIndex>,
    target_frames: Vec<FrameNodeIndex>,
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
    storage.frame_has_empty_effective_clip.clear();
    storage.applied_frames.clear();
    storage.target_frames.clear();
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
    painter: &'a mut Painter,
    palette: ReplayPaletteStorage,
    frame_has_empty_effective_clip: Vec<bool>,
    spatial_dependency_order: Vec<u32>,
    tree_has_sorting_contexts: bool,
    replay_base_matrix: FloatMatrix4x4,
    // The palette entry the canvas matrix currently equals, if known; popping a frame resets the
    // matrix to its save point, so unwinding applied frames invalidates it. Recorded streams
    // contain no matrix-mutating commands, so playing commands never invalidates the cache.
    current_ctm_space: Option<SpatialNodeIndex>,
    // Applied and target frames track the visual-context chain. Semantic frames push and pop canvas state;
    // metadata-only frames do not. A context without a frame takes its coordinates from the palette.
    applied_frames: Vec<FrameNodeIndex>,
    target_frames: Vec<FrameNodeIndex>,
    applied_context: Option<ContextRef>,
    applied_mask_frame_count: usize,
}

fn replay_clip_of(clip: &crate::painting::visual_context::ClipData) -> ReplayClip {
    ReplayClip {
        rect: clip.rect,
        corner_radii: clip.corner_radii,
        mode: clip.mode,
    }
}

fn replay_layer_of(effects: &crate::painting::visual_context::EffectsData, frame: FrameNodeIndex) -> ReplayLayer {
    let (filter_bytes, filter_bytes_size) = match &effects.filter {
        Some(bytes) => (bytes.as_ptr(), bytes.len()),
        None => (std::ptr::null(), 0),
    };
    ReplayLayer {
        opacity: effects.opacity,
        blend_mode: effects.blend_mode,
        filter_bytes,
        filter_bytes_size,
        frame,
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

    fn build_target_frames(&mut self, target_frame: FrameNodeIndex) {
        let tree = self.tree;
        self.target_frames.clear();
        let mut frame = target_frame;
        while !frame.is_none() {
            self.target_frames.push(frame);
            frame = tree.frame_nodes[frame.0 as usize].parent;
        }
        self.target_frames.reverse();
    }

    fn restore_to_length(&mut self, length: usize) {
        let tree = self.tree;
        self.applied_context = None;
        while self.applied_frames.len() > length {
            let frame_index = self.applied_frames.pop().expect("applied frames are not empty");
            let frame_node = &tree.frame_nodes[frame_index.0 as usize];
            let mask = if self.applied_mask_frame_count > 0 {
                match &frame_node.data {
                    FrameData::Mask(mask) => Some(mask),
                    _ => None,
                }
            } else {
                None
            };
            if let Some(mask) = mask {
                self.applied_mask_frame_count -= 1;
                self.ensure_ctm_space(frame_node.spatial);
                self.painter.pop_mask(&replay_mask_of(mask), frame_index);
            } else if !matches!(frame_node.data, FrameData::BackgroundColorAnimation) {
                self.painter.pop();
            }
            self.current_ctm_space = None;
        }
    }

    // OPTIMIZATION: When walking down to layer-pushing frames (effects and masks), check culling before pushing
    //               each one. Effects don't affect clip state and a mask push only narrows it, so testing against
    //               the pre-push clip is conservative and valid. This avoids expensive saveLayer/restore cycles
    //               for off-screen elements.
    fn switch_to_context(&mut self, target: ContextRef, bounding_rect: Option<IntRect>) -> SwitchResult {
        if self.applied_context == Some(target) {
            return SwitchResult::Switched;
        }

        let tree = self.tree;
        self.build_target_frames(target.frame);

        let common_prefix_length = self
            .applied_frames
            .iter()
            .zip(&self.target_frames)
            .take_while(|(applied, target)| applied == target)
            .count();

        self.restore_to_length(common_prefix_length);

        for i in common_prefix_length..self.target_frames.len() {
            let frame_index = self.target_frames[i];
            let frame_node = &tree.frame_nodes[frame_index.0 as usize];
            let pushes_layer = matches!(frame_node.data, FrameData::Effects(_) | FrameData::Mask(_));
            if pushes_layer && let Some(bounding_rect) = bounding_rect {
                let mut culled_by_layer_frame = bounding_rect.is_empty();
                if !culled_by_layer_frame {
                    self.ensure_ctm_space(target.spatial);
                    culled_by_layer_frame = self.painter.would_be_fully_clipped_by_painter(bounding_rect);
                }
                if culled_by_layer_frame {
                    self.restore_to_length(common_prefix_length);
                    // The canvas is unwound to the shared prefix; clearing the applied context
                    // keeps the fast path from reusing the pre-cull context while the frame
                    // vector still enables prefix reuse on the next switch.
                    return SwitchResult::CulledByEffect;
                }
            }
            self.ensure_ctm_space(frame_node.spatial);
            match &frame_node.data {
                FrameData::BackgroundColorAnimation => {}
                FrameData::Clip(clip) => self.painter.push_clip(&replay_clip_of(clip)),
                FrameData::ClipPath(clip_path) => self.painter.push_clip_path(&clip_path.path, clip_path.fill_rule),
                FrameData::Effects(effects) => self.painter.push_layer(&replay_layer_of(effects, frame_index)),
                FrameData::Mask(mask) => {
                    self.painter.push_mask(&replay_mask_of(mask));
                    self.applied_mask_frame_count += 1;
                }
                FrameData::Dead => unreachable!("a run never records under a tombstoned frame"),
            }
            self.applied_frames.push(frame_index);
        }

        self.applied_context = Some(target);
        SwitchResult::Switched
    }

    // A run enters its context once. Only a run whose ink bounds are known may be skipped as a
    // whole, and only such a run offers its bounds to the layer-frame cull. Skipping a run with
    // nothing to draw before entering its context spares the frame pushes.
    fn execute_run(&mut self, run_index: usize) {
        let run = self.command_runs[run_index];
        if self.palette.backface_culled[run.context.spatial.0 as usize] {
            return;
        }
        if !run.context.frame.is_none() && self.frame_has_empty_effective_clip[run.context.frame.0 as usize] {
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
            let root_isolation_frame = tree.root_isolation_frame;
            let plane_clip_base_length = usize::from(root_isolation_frame.is_some());
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
                &self.frame_has_empty_effective_clip,
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
                        if let Some(root_isolation_frame) = root_isolation_frame {
                            self.switch_to_context(
                                ContextRef {
                                    spatial: tree.frame_nodes[root_isolation_frame.0 as usize].spatial,
                                    frame: root_isolation_frame,
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
    scroll_offsets: &[FloatPoint],
    painter: &mut impl ReplayPainter,
) {
    let replay_base_matrix = painter.canvas_matrix();
    let mut scratch = take_replay_scratch_storage();
    tree.fill_frames_with_empty_effective_clip(&mut scratch.frame_has_empty_effective_clip);
    let mut driver = ReplayDriver {
        tree,
        command_runs,
        painter,
        palette: scratch.palette,
        frame_has_empty_effective_clip: scratch.frame_has_empty_effective_clip,
        spatial_dependency_order: tree.spatial_dependency_order(),
        tree_has_sorting_contexts: tree.spatial_nodes.iter().any(|node| {
            matches!(&node.data, SpatialData::Transform(transform) if transform.sorting_context_root_index.is_some())
        }),
        replay_base_matrix,
        current_ctm_space: None,
        applied_frames: scratch.applied_frames,
        target_frames: scratch.target_frames,
        applied_context: None,
        applied_mask_frame_count: 0,
    };
    driver.build_transform_palette(scroll_offsets);
    driver.execute();
    return_replay_scratch_storage(ReplayScratchStorage {
        palette: std::mem::take(&mut driver.palette),
        frame_has_empty_effective_clip: std::mem::take(&mut driver.frame_has_empty_effective_clip),
        applied_frames: std::mem::take(&mut driver.applied_frames),
        target_frames: std::mem::take(&mut driver.target_frames),
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::node_data::NodeSlotId;
    use crate::painting::display_list::commands::VISUAL_VIEWPORT_NODE_INDEX;
    use crate::painting::visual_context::{
        BackfaceVisibilityData, ClipData, ClipMode, EffectsData, FrameData, MaskData, MaskLayerOrigin, SpatialData,
        TransformData, TransformDataRole,
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
        PopMask(FrameNodeIndex),
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
        fn pop_mask(&mut self, _mask: &ReplayMask, frame: FrameNodeIndex) {
            self.events.push(PainterEvent::PopMask(frame));
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

    fn clip(rect: FloatRect) -> FrameData {
        FrameData::Clip(ClipData {
            rect,
            corner_radii: CornerRadii::default(),
            mode: ClipMode::Intersect,
        })
    }

    fn effects(opacity: f32) -> FrameData {
        FrameData::Effects(EffectsData {
            opacity,
            blend_mode: CompositingAndBlendingOperator::Normal,
            filter: None,
        })
    }

    fn run(spatial: SpatialNodeIndex, frame: FrameNodeIndex, ink_bounds: IntRect) -> DisplayListCommandRun {
        DisplayListCommandRun {
            offset: 0,
            size: 0,
            context: ContextRef { spatial, frame },
            ink_bounds,
            has_unbounded_draw: false,
            has_compositor_metadata: false,
        }
    }

    fn visible_bounds() -> IntRect {
        IntRect::new(0, 0, 10, 10)
    }

    fn replay(tree: &VisualContextTree, runs: &[DisplayListCommandRun], painter: &mut RecordingPainter) {
        replay_display_list(tree, runs, &[], painter);
    }

    #[test]
    fn runs_sharing_a_frame_prefix_push_it_once_and_re_enter_their_space_after_a_pop() {
        let mut tree = identity_tree();
        let clip_frame = tree.append_frame(
            clip(FloatRect::new(1.0, 2.0, 3.0, 4.0)),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let layer_frame = tree.append_frame(effects(0.5), clip_frame, VISUAL_VIEWPORT_NODE_INDEX);
        let runs = [
            run(VISUAL_VIEWPORT_NODE_INDEX, clip_frame, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, layer_frame, visible_bounds()),
            run(VISUAL_VIEWPORT_NODE_INDEX, clip_frame, visible_bounds()),
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
        let empty_clip = tree.append_frame(
            clip(FloatRect::default()),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let runs = [run(VISUAL_VIEWPORT_NODE_INDEX, empty_clip, visible_bounds())];
        let mut painter = RecordingPainter::new();
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        assert_eq!(painter.events, vec![PainterEvent::SetMatrix(base)]);
    }

    #[test]
    fn a_fully_clipped_layer_frame_is_culled_and_the_next_run_pushes_the_chain_again() {
        let mut tree = identity_tree();
        let clip_frame = tree.append_frame(
            clip(FloatRect::new(0.0, 0.0, 50.0, 50.0)),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let layer_frame = tree.append_frame(effects(0.25), clip_frame, VISUAL_VIEWPORT_NODE_INDEX);
        let off_screen = IntRect::new(999, 0, 10, 10);
        let runs = [
            run(VISUAL_VIEWPORT_NODE_INDEX, layer_frame, off_screen),
            run(VISUAL_VIEWPORT_NODE_INDEX, layer_frame, visible_bounds()),
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
    fn mask_frames_pop_with_their_frame_index() {
        let mut tree = identity_tree();
        let mask_frame = tree.append_frame(
            FrameData::Mask(MaskData {
                rect: IntRect::new(0, 0, 20, 20),
                kind: MaskKind::Alpha,
                origin: MaskLayerOrigin::CssMaskLayers,
            }),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let runs = [run(VISUAL_VIEWPORT_NODE_INDEX, mask_frame, visible_bounds())];
        let mut painter = RecordingPainter::new();
        let base = painter.base_matrix;
        replay(&tree, &runs, &mut painter);
        assert_eq!(
            painter.events,
            vec![
                PainterEvent::SetMatrix(base),
                PainterEvent::PushMask(IntRect::new(0, 0, 20, 20)),
                PainterEvent::Run(0),
                PainterEvent::PopMask(mask_frame),
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
            run(marker, FrameNodeIndex::NONE, visible_bounds()),
            run(scroll_node, FrameNodeIndex::NONE, visible_bounds()),
        ];
        let mut scroll_offsets = vec![FloatPoint::default(); 4];
        scroll_offsets[scroll_node.0 as usize] = FloatPoint { x: 0.0, y: -30.7 };
        let mut painter = RecordingPainter::new();
        let base = painter.base_matrix;
        replay_display_list(&tree, &runs, &scroll_offsets, &mut painter);
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
        let rotate_y = |degrees: f32| {
            let (sin, cos) = degrees.to_radians().sin_cos();
            FloatMatrix4x4 {
                elements: [
                    [cos, 0.0, sin, 0.0],
                    [0.0, 1.0, 0.0, 0.0],
                    [-sin, 0.0, cos, 0.0],
                    [0.0, 0.0, 0.0, 1.0],
                ],
            }
        };
        let first_plane = tree.append_spatial(transform(rotate_y(45.0), Some(context_root)), context_root);
        let second_plane = tree.append_spatial(transform(rotate_y(-45.0), Some(context_root)), context_root);
        let plane_bounds = IntRect::new(-50, -50, 100, 100);
        let runs = [
            run(first_plane, FrameNodeIndex::NONE, plane_bounds),
            run(second_plane, FrameNodeIndex::NONE, plane_bounds),
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
}
