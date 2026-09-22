/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The compositor animations a visual context tree carries, and the trees derived by sampling them.

use std::rc::Rc;

use super::{EffectNodeData, EffectNodeIndex, EffectsData, SpatialData, SpatialNodeIndex, VisualContextTree};
use crate::painting::host::{
    FfiVisualAnimationSummary, FfiVisualAnimationTargetKind, FfiVisualAnimationTransformOperationKind,
};
use crate::painting::visual_animation::{VisualAnimation, VisualAnimationSample, VisualAnimationValue};

/// What a frame scheduler needs to bound the content the running animations move: the nodes of the
/// animations whose swept area is bounded, sorted by how the bound is computed, and whether every
/// running animation is one of them.
pub struct VisualAnimationExtents {
    /// Spatial nodes of the running animations that only rotate in the plane.
    pub rotation_nodes: Vec<SpatialNodeIndex>,
    /// Effect nodes of the running opacity animations, which never move content.
    pub opacity_nodes: Vec<EffectNodeIndex>,
    pub has_finished_animation: bool,
    pub has_unfinished_finite_animation: bool,
    pub all_running_animations_are_bounded: bool,
}

impl VisualContextTree {
    pub fn visual_animations(&self) -> &[VisualAnimation] {
        &self.visual_animations
    }

    pub fn has_visual_animations(&self) -> bool {
        !self.visual_animations.is_empty()
    }

    pub fn set_visual_animations(&mut self, animations: Vec<VisualAnimation>) {
        self.visual_animations = Rc::from(animations);
    }

    pub fn clear_visual_animations(&mut self) {
        if self.has_visual_animations() {
            self.visual_animations = Rc::from(Vec::new());
        }
    }

    fn effects_mut(&mut self, node_index: u32) -> Option<&mut EffectsData> {
        match self
            .effect_nodes
            .get_mut(node_index as usize)
            .map(|node| &mut node.data)
        {
            Some(EffectNodeData::Effects(effects)) => Some(effects),
            _ => None,
        }
    }

    /// A copy of the tree whose nodes carry the values the animations take at `sample_time_ns`. An
    /// animation that has no value at that time, because it is dormant or its timing cannot be
    /// resolved, leaves its nodes as the main thread published them.
    pub fn with_visual_animation_samples(&self, sample_time_ns: i64) -> Self {
        let mut sampled = self.clone();
        sampled.sampled_background_colors.clear();
        for animation in self.visual_animations.iter() {
            let Some(sample) = animation.sample(animation.elapsed_since_anchor_ns(sample_time_ns)) else {
                continue;
            };
            for &node_index in &animation.node_indices {
                match &sample {
                    VisualAnimationSample::Opacity(opacity) => {
                        if let Some(effects) = sampled.effects_mut(node_index) {
                            effects.opacity = *opacity;
                        }
                    }
                    VisualAnimationSample::BackgroundColor(color) => {
                        sampled.sampled_background_colors.insert(node_index, *color);
                    }
                    VisualAnimationSample::Filter(filter) => {
                        if let Some(effects) = sampled.effects_mut(node_index) {
                            effects.filter = filter.clone();
                        }
                    }
                    VisualAnimationSample::Transform(matrix) => {
                        if let Some(SpatialData::Transform(transform)) = sampled
                            .spatial_nodes
                            .get_mut(node_index as usize)
                            .map(|node| &mut node.data)
                        {
                            transform.matrix = *matrix;
                        }
                    }
                }
            }
        }
        sampled
    }

    pub fn has_active_visual_animation_at(&self, sample_time_ns: i64) -> bool {
        self.visual_animations
            .iter()
            .any(|animation| animation.is_active_at(sample_time_ns))
    }

    pub fn visual_animations_target_valid_nodes(&self) -> bool {
        self.visual_animations
            .iter()
            .all(|animation| self.visual_animation_targets_are_valid(animation.target_kind, &animation.node_indices))
    }

    pub fn visual_animation_summary(&self) -> FfiVisualAnimationSummary {
        let Some(first) = self.visual_animations.first() else {
            return FfiVisualAnimationSummary {
                count: 0,
                local_time_at_anchor_ms_of_first: 0.0,
                share_timing_anchor: true,
                targets_are_valid: true,
            };
        };
        FfiVisualAnimationSummary {
            count: self.visual_animations.len(),
            local_time_at_anchor_ms_of_first: first.local_time_at_anchor_ms,
            share_timing_anchor: self.visual_animations.iter().all(|animation| {
                animation.monotonic_time_at_anchor_ns == first.monotonic_time_at_anchor_ns
                    && animation.local_time_at_anchor_ms == first.local_time_at_anchor_ms
            }),
            targets_are_valid: self.visual_animations_target_valid_nodes(),
        }
    }

    /// One flag per spatial node: whether a transform animation moves the node or an ancestor.
    pub fn spatial_nodes_in_subtrees_of_transform_animations(&self) -> Vec<bool> {
        let roots: Vec<SpatialNodeIndex> = self
            .visual_animations
            .iter()
            .filter(|animation| animation.target_kind == FfiVisualAnimationTargetKind::Transform)
            .flat_map(|animation| animation.node_indices.iter().map(|&index| SpatialNodeIndex(index)))
            .collect();
        self.spatial_nodes_in_subtrees_of(&roots)
    }

    pub fn visual_animation_extents_at(&self, sample_time_ns: i64) -> VisualAnimationExtents {
        let mut extents = VisualAnimationExtents {
            rotation_nodes: Vec::new(),
            opacity_nodes: Vec::new(),
            has_finished_animation: false,
            has_unfinished_finite_animation: false,
            all_running_animations_are_bounded: true,
        };
        for animation in self.visual_animations.iter() {
            if animation.iteration_count.is_finite() {
                if animation.has_finished_at(sample_time_ns) {
                    extents.has_finished_animation = true;
                    continue;
                }
                extents.has_unfinished_finite_animation = true;
            }
            match animation.target_kind {
                FfiVisualAnimationTargetKind::Opacity => extents
                    .opacity_nodes
                    .extend(animation.node_indices.iter().map(|&index| EffectNodeIndex(index))),
                FfiVisualAnimationTargetKind::Transform
                    if animation.keyframes.iter().all(only_rotates_in_the_plane) =>
                {
                    extents
                        .rotation_nodes
                        .extend(animation.node_indices.iter().map(|&index| SpatialNodeIndex(index)));
                }
                _ => {
                    extents.all_running_animations_are_bounded = false;
                    return extents;
                }
            }
        }
        extents
    }
}

fn only_rotates_in_the_plane(keyframe: &crate::painting::visual_animation::VisualAnimationKeyframe) -> bool {
    match &keyframe.value {
        VisualAnimationValue::Transform(operations) => operations.iter().all(|operation| {
            matches!(
                operation.kind,
                FfiVisualAnimationTransformOperationKind::Rotate | FfiVisualAnimationTransformOperationKind::RotateZ
            )
        }),
        _ => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::easing::Easing;
    use crate::painting::visual_animation::{VisualAnimationKeyframe, VisualAnimationTransformOperation};
    use crate::painting::visual_context::{
        ClipNodeIndex, EffectsData, TransformData, TransformDataRole, VISUAL_VIEWPORT_NODE_INDEX,
    };
    use libgfx_rust::{CompositingAndBlendingOperator, FloatMatrix4x4, FloatPoint, translation_matrix};

    fn transform_data(matrix: FloatMatrix4x4) -> TransformData {
        TransformData {
            matrix,
            origin: FloatPoint { x: 0.0, y: 0.0 },
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
        }
    }

    fn opacity_effect() -> EffectNodeData {
        EffectNodeData::Effects(EffectsData {
            opacity: 1.0,
            blend_mode: CompositingAndBlendingOperator::Normal,
            filter: None,
            backdrop_filter: None,
        })
    }

    fn keyframe(offset: f64, value: VisualAnimationValue) -> VisualAnimationKeyframe {
        VisualAnimationKeyframe {
            offset,
            easing: Easing::default(),
            value,
        }
    }

    fn translate_x(pixels: f32) -> VisualAnimationValue {
        VisualAnimationValue::Transform(vec![
            VisualAnimationTransformOperation::new(FfiVisualAnimationTransformOperationKind::TranslateX, &[pixels])
                .unwrap(),
        ])
    }

    fn milliseconds(value: i64) -> i64 {
        value * 1_000_000
    }

    #[test]
    fn sampling_derives_a_tree_and_leaves_the_source_untouched() {
        let mut tree = VisualContextTree::create(transform_data(FloatMatrix4x4::identity()));
        let spatial = tree.append_spatial(
            SpatialData::Transform(transform_data(FloatMatrix4x4::identity())),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let effect = tree.append_effect(opacity_effect(), EffectNodeIndex::NONE, spatial, ClipNodeIndex::NONE);
        tree.set_visual_animations(vec![
            VisualAnimation {
                target_kind: FfiVisualAnimationTargetKind::Transform,
                node_indices: vec![spatial.0],
                iteration_duration_ms: 1000.0,
                keyframes: vec![keyframe(0.0, translate_x(0.0)), keyframe(1.0, translate_x(20.0))],
                ..VisualAnimation::default()
            },
            VisualAnimation {
                target_kind: FfiVisualAnimationTargetKind::Opacity,
                node_indices: vec![effect.0],
                iteration_duration_ms: 1000.0,
                keyframes: vec![
                    keyframe(0.0, VisualAnimationValue::Opacity(0.0)),
                    keyframe(1.0, VisualAnimationValue::Opacity(1.0)),
                ],
                ..VisualAnimation::default()
            },
        ]);
        assert!(tree.has_visual_animations());
        assert!(tree.visual_animations_target_valid_nodes());

        let sampled = tree.with_visual_animation_samples(milliseconds(500));
        assert_eq!(sampled.structural_epoch, tree.structural_epoch);
        assert_eq!(sampled.visual_animations().len(), 2);
        let SpatialData::Transform(sampled_transform) = &sampled.spatial_nodes[spatial.0 as usize].data else {
            panic!("not a transform node");
        };
        assert_eq!(sampled_transform.matrix, translation_matrix(10.0, 0.0, 0.0));
        assert_eq!(sampled.effects_opacity(effect), Some(0.5));

        let SpatialData::Transform(source_transform) = &tree.spatial_nodes[spatial.0 as usize].data else {
            panic!("not a transform node");
        };
        assert_eq!(source_transform.matrix, FloatMatrix4x4::identity());
        assert_eq!(tree.effects_opacity(effect), Some(1.0));

        let flags = tree.spatial_nodes_in_subtrees_of_transform_animations();
        assert_eq!(flags, vec![false, true]);
    }

    #[test]
    fn a_dormant_animation_leaves_its_node_as_published() {
        let mut tree = VisualContextTree::create(transform_data(FloatMatrix4x4::identity()));
        let spatial = tree.append_spatial(
            SpatialData::Transform(transform_data(translation_matrix(5.0, 0.0, 0.0))),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        tree.set_visual_animations(vec![VisualAnimation {
            target_kind: FfiVisualAnimationTargetKind::Transform,
            node_indices: vec![spatial.0],
            start_delay_ms: 1000.0,
            iteration_duration_ms: 1000.0,
            keyframes: vec![keyframe(0.0, translate_x(0.0)), keyframe(1.0, translate_x(20.0))],
            ..VisualAnimation::default()
        }]);

        assert!(!tree.has_active_visual_animation_at(milliseconds(500)));
        assert!(tree.has_active_visual_animation_at(milliseconds(1500)));
        let sampled = tree.with_visual_animation_samples(milliseconds(500));
        let SpatialData::Transform(sampled_transform) = &sampled.spatial_nodes[spatial.0 as usize].data else {
            panic!("not a transform node");
        };
        assert_eq!(sampled_transform.matrix, translation_matrix(5.0, 0.0, 0.0));
    }

    #[test]
    fn the_summary_reports_the_shared_anchor_and_the_targets() {
        let mut tree = VisualContextTree::create(transform_data(FloatMatrix4x4::identity()));
        assert_eq!(tree.visual_animation_summary().count, 0);
        assert!(tree.visual_animation_summary().share_timing_anchor);
        assert!(tree.visual_animation_summary().targets_are_valid);

        let spatial = tree.append_spatial(
            SpatialData::Transform(transform_data(FloatMatrix4x4::identity())),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let animation = VisualAnimation {
            target_kind: FfiVisualAnimationTargetKind::Transform,
            node_indices: vec![spatial.0],
            monotonic_time_at_anchor_ns: 10,
            local_time_at_anchor_ms: 20.0,
            iteration_duration_ms: 1000.0,
            keyframes: vec![keyframe(0.0, translate_x(0.0)), keyframe(1.0, translate_x(20.0))],
            ..VisualAnimation::default()
        };
        tree.set_visual_animations(vec![
            animation.clone(),
            VisualAnimation {
                local_time_at_anchor_ms: 30.0,
                ..animation.clone()
            },
        ]);
        let summary = tree.visual_animation_summary();
        assert_eq!(summary.count, 2);
        assert_eq!(summary.local_time_at_anchor_ms_of_first, 20.0);
        assert!(!summary.share_timing_anchor);
        assert!(summary.targets_are_valid);

        tree.set_visual_animations(vec![VisualAnimation {
            node_indices: vec![spatial.0],
            target_kind: FfiVisualAnimationTargetKind::Opacity,
            keyframes: vec![
                keyframe(0.0, VisualAnimationValue::Opacity(0.0)),
                keyframe(1.0, VisualAnimationValue::Opacity(1.0)),
            ],
            ..animation
        }]);
        assert!(!tree.visual_animation_summary().targets_are_valid);

        tree.clear_visual_animations();
        assert!(!tree.has_visual_animations());
    }

    #[test]
    fn extents_sort_running_animations_by_how_their_content_is_bounded() {
        let mut tree = VisualContextTree::create(transform_data(FloatMatrix4x4::identity()));
        let rotated = tree.append_spatial(
            SpatialData::Transform(transform_data(FloatMatrix4x4::identity())),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let effect = tree.append_effect(opacity_effect(), EffectNodeIndex::NONE, rotated, ClipNodeIndex::NONE);
        let rotate = |angle: f32| {
            VisualAnimationValue::Transform(vec![
                VisualAnimationTransformOperation::new(FfiVisualAnimationTransformOperationKind::Rotate, &[angle])
                    .unwrap(),
            ])
        };
        let rotation = VisualAnimation {
            target_kind: FfiVisualAnimationTargetKind::Transform,
            node_indices: vec![rotated.0],
            iteration_duration_ms: 1000.0,
            iteration_count: 1.0,
            keyframes: vec![keyframe(0.0, rotate(0.0)), keyframe(1.0, rotate(1.0))],
            ..VisualAnimation::default()
        };
        let opacity = VisualAnimation {
            target_kind: FfiVisualAnimationTargetKind::Opacity,
            node_indices: vec![effect.0],
            iteration_duration_ms: 1000.0,
            keyframes: vec![
                keyframe(0.0, VisualAnimationValue::Opacity(0.0)),
                keyframe(1.0, VisualAnimationValue::Opacity(1.0)),
            ],
            ..VisualAnimation::default()
        };
        tree.set_visual_animations(vec![rotation.clone(), opacity.clone()]);

        let running = tree.visual_animation_extents_at(milliseconds(500));
        assert!(running.all_running_animations_are_bounded);
        assert_eq!(running.rotation_nodes, vec![rotated]);
        assert_eq!(running.opacity_nodes, vec![effect]);
        assert!(running.has_unfinished_finite_animation);
        assert!(!running.has_finished_animation);

        let finished = tree.visual_animation_extents_at(milliseconds(1000));
        assert!(finished.all_running_animations_are_bounded);
        assert!(finished.rotation_nodes.is_empty());
        assert_eq!(finished.opacity_nodes, vec![effect]);
        assert!(!finished.has_unfinished_finite_animation);
        assert!(finished.has_finished_animation);

        tree.set_visual_animations(vec![
            VisualAnimation {
                keyframes: vec![keyframe(0.0, translate_x(0.0)), keyframe(1.0, translate_x(20.0))],
                ..rotation
            },
            opacity,
        ]);
        assert!(
            !tree
                .visual_animation_extents_at(milliseconds(500))
                .all_running_animations_are_bounded
        );
    }
}
