/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The compositor animations a visual context tree carries, and the trees derived by sampling them.

use super::{
    ClipBehavior, ClipNodeIndex, ContextRef, EffectNodeData, EffectNodeIndex, EffectsData, SpatialData,
    SpatialNodeIndex, VisualContextTree, WorldTransforms,
};
use crate::filter_bytes::filter_functions_graph;
use crate::host::{FfiVisualAnimationSummary, FfiVisualAnimationTargetKind, FfiVisualAnimationTransformOperationKind};
use crate::visual_animation::{VisualAnimation, VisualAnimationSample, VisualAnimationValue};
use libgfx_rust::FloatPoint;
use libgfx_rust::filter::Filter;

/// What the animations of a tree animate, per node, worked out once as they are set, so that the copies of the tree
/// that share the animations share it too.
#[derive(Default)]
pub(crate) struct VisualAnimationTargets {
    effect_targets: Vec<AnimatedEffect>,
    transform_animation_leaves_plane_by_spatial_index: Vec<bool>,
}

#[derive(Clone, Copy, Default)]
struct AnimatedEffect {
    opacity_or_filter_is_animated: bool,
    filter_is_animated: bool,
    some_keyframe_filter_spreads: bool,
}

impl VisualAnimationTargets {
    fn of(animations: &[VisualAnimation]) -> Self {
        let mut targets = Self::default();
        for animation in animations {
            match animation.target_kind {
                FfiVisualAnimationTargetKind::Opacity | FfiVisualAnimationTargetKind::Filter => {
                    let is_filter = animation.target_kind == FfiVisualAnimationTargetKind::Filter;
                    let some_keyframe_filter_spreads = is_filter
                        && animation.keyframes.iter().any(|keyframe| {
                            matches!(&keyframe.value, VisualAnimationValue::Filter(functions)
                                if filter_functions_graph(functions.iter().copied().map(Filter::from))
                                    .is_some_and(|graph| graph.may_affect_output_bounds()))
                        });
                    for &node_index in &animation.node_indices {
                        let index = node_index as usize;
                        if targets.effect_targets.len() <= index {
                            targets.effect_targets.resize(index + 1, AnimatedEffect::default());
                        }
                        let target = &mut targets.effect_targets[index];
                        target.opacity_or_filter_is_animated = true;
                        target.filter_is_animated |= is_filter;
                        target.some_keyframe_filter_spreads |= some_keyframe_filter_spreads;
                    }
                }
                FfiVisualAnimationTargetKind::Transform => {
                    if crate::display_list::animated_transform_spaces::transform_animation_stays_in_plane(animation) {
                        continue;
                    }
                    for &node_index in &animation.node_indices {
                        let index = node_index as usize;
                        let leaves_plane = &mut targets.transform_animation_leaves_plane_by_spatial_index;
                        if leaves_plane.len() <= index {
                            leaves_plane.resize(index + 1, false);
                        }
                        leaves_plane[index] = true;
                    }
                }
                FfiVisualAnimationTargetKind::BackgroundColor => {}
            }
        }
        targets
    }

    fn effect(&self, effect: EffectNodeIndex) -> AnimatedEffect {
        self.effect_targets.get(effect.0 as usize).copied().unwrap_or_default()
    }
}

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
        self.visual_animation_targets = std::sync::Arc::new(VisualAnimationTargets::of(&animations));
        self.visual_animations = std::sync::Arc::from(animations);
    }

    pub fn clear_visual_animations(&mut self) {
        if self.has_visual_animations() {
            self.visual_animations = std::sync::Arc::from(Vec::new());
            self.visual_animation_targets = std::sync::Arc::default();
        }
    }

    /// Whether an opacity or filter animation of the tree animates the effect node.
    pub fn effect_opacity_or_filter_is_animated(&self, effect: EffectNodeIndex) -> bool {
        self.visual_animation_targets
            .effect(effect)
            .opacity_or_filter_is_animated
    }

    /// Whether a filter animation of the tree animates the effect node.
    pub fn effect_filter_is_animated(&self, effect: EffectNodeIndex) -> bool {
        self.visual_animation_targets.effect(effect).filter_is_animated
    }

    /// Whether the filter a keyframe of a filter animation of the effect node gives it can spread what it filters
    /// outside its bounds.
    pub fn some_keyframe_filter_of_effect_spreads(&self, effect: EffectNodeIndex) -> bool {
        self.visual_animation_targets
            .effect(effect)
            .some_keyframe_filter_spreads
    }

    /// Whether a transform animation of the tree that turns what it transforms out of the plane at some sample
    /// animates the spatial node.
    pub fn transform_animation_of_node_leaves_plane(&self, node: SpatialNodeIndex) -> bool {
        self.visual_animation_targets
            .transform_animation_leaves_plane_by_spatial_index
            .get(node.0 as usize)
            .copied()
            .unwrap_or(false)
    }

    /// Whether a transform animation of the tree animates a plane of a 3D sorting context, whose samples can hold the
    /// plane in the plane of its context where the tree turns it out of it, or the other way around.
    pub fn transform_animation_animates_plane_of_sorting_context(&self) -> bool {
        self.visual_animations
            .iter()
            .filter(|animation| animation.target_kind == FfiVisualAnimationTargetKind::Transform)
            .flat_map(|animation| animation.node_indices.iter())
            .any(|&node_index| {
                matches!(self.spatial_nodes.get(node_index as usize).map(|node| &node.data),
                    Some(SpatialData::Transform(transform)) if transform.sorting_context_root_index.is_some())
            })
    }

    pub fn shared_visual_animations(&self) -> std::sync::Arc<[VisualAnimation]> {
        self.visual_animations.clone()
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
        sampled.animation_samples_taken_from_copy =
            super::AnimationSamplesTakenFromCopy(Some(self.copy_without_animation_samples()));
        sampled.sampled_background_color_by_effect_index.clear();
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
                        sampled
                            .sampled_background_color_by_effect_index
                            .insert(node_index, *color);
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

    /// Whether `screen_point` lies in the bounds of a node that a transform animation running at `sample_time_ns`
    /// moves, through the values the nodes hold: those of this tree sampled at that time, where a frame shows it.
    pub fn running_transform_animation_covers_point(
        &self,
        sample_time_ns: i64,
        screen_point: FloatPoint,
        scroll_offsets: &[FloatPoint],
    ) -> bool {
        let world = std::cell::OnceCell::new();
        self.visual_animations.iter().any(|animation| {
            animation.target_kind == FfiVisualAnimationTargetKind::Transform
                && animation.is_active_at(sample_time_ns)
                && animation.node_indices.iter().any(|&node_index| {
                    let Some(SpatialData::Transform(transform)) =
                        self.spatial_nodes.get(node_index as usize).map(|node| &node.data)
                    else {
                        return false;
                    };
                    let context = ContextRef {
                        spatial: SpatialNodeIndex(node_index),
                        clip: ClipNodeIndex::NONE,
                        effect: EffectNodeIndex::NONE,
                    };
                    let world = world.get_or_init(|| WorldTransforms::new(self, scroll_offsets, None));
                    self.transform_point_for_hit_test(world, context, screen_point, ClipBehavior::Ignore)
                        .is_some_and(|local_point| transform.bounds.contains_point(local_point))
                })
        })
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
            if animation.is_finite() {
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

fn only_rotates_in_the_plane(keyframe: &crate::visual_animation::VisualAnimationKeyframe) -> bool {
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
    use crate::animation_timing::{AnimationEffectTiming, TimeValue};
    use crate::easing::Easing;
    use crate::visual_animation::{VisualAnimationKeyframe, VisualAnimationTransformOperation};
    use crate::visual_context::{
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
            bounds: libgfx_rust::FloatRect::default(),
        }
    }

    fn opacity_effect() -> EffectNodeData {
        EffectNodeData::Effects(EffectsData {
            isolation: false,
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
                timing: AnimationEffectTiming {
                    iteration_duration: TimeValue::milliseconds(1000.0),
                    ..VisualAnimation::default().timing
                },
                keyframes: vec![keyframe(0.0, translate_x(0.0)), keyframe(1.0, translate_x(20.0))],
                ..VisualAnimation::default()
            },
            VisualAnimation {
                target_kind: FfiVisualAnimationTargetKind::Opacity,
                node_indices: vec![effect.0],
                timing: AnimationEffectTiming {
                    iteration_duration: TimeValue::milliseconds(1000.0),
                    ..VisualAnimation::default().timing
                },
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
            timing: AnimationEffectTiming {
                start_delay: TimeValue::milliseconds(1000.0),
                iteration_duration: TimeValue::milliseconds(1000.0),
                ..VisualAnimation::default().timing
            },
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
    fn a_running_transform_animation_covers_a_point_in_the_bounds_it_moves() {
        let mut tree = VisualContextTree::create(transform_data(FloatMatrix4x4::identity()));
        // A box from x 150 to 1150 slides 150 pixels a second to the left: it covers x 50 from 2/3 s to 7 1/3 s in.
        let spatial = tree.append_spatial(
            SpatialData::Transform(TransformData {
                bounds: libgfx_rust::FloatRect::new(150.0, 0.0, 1000.0, 100.0),
                ..transform_data(FloatMatrix4x4::identity())
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        tree.set_visual_animations(vec![VisualAnimation {
            target_kind: FfiVisualAnimationTargetKind::Transform,
            node_indices: vec![spatial.0],
            timing: AnimationEffectTiming {
                iteration_duration: TimeValue::milliseconds(10000.0),
                iteration_count: 1.0,
                ..VisualAnimation::default().timing
            },
            keyframes: vec![keyframe(0.0, translate_x(0.0)), keyframe(1.0, translate_x(-1500.0))],
            ..VisualAnimation::default()
        }]);
        let point = FloatPoint { x: 50.0, y: 50.0 };
        let covers_at = |time_ns: i64| {
            tree.with_visual_animation_samples(time_ns)
                .running_transform_animation_covers_point(time_ns, point, &[])
        };
        assert!(!covers_at(milliseconds(0)));
        assert!(covers_at(milliseconds(2000)));
        assert!(!covers_at(milliseconds(9000)));
        // The animation over, the node holds the value the main thread published, which has the box beside the point.
        assert!(!covers_at(milliseconds(11000)));
        // The tree answers for the values it holds: unsampled, the box is where the main thread published it.
        assert!(!tree.running_transform_animation_covers_point(milliseconds(2000), point, &[]));
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
            timing: AnimationEffectTiming {
                iteration_duration: TimeValue::milliseconds(1000.0),
                ..VisualAnimation::default().timing
            },
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
            timing: AnimationEffectTiming {
                iteration_duration: TimeValue::milliseconds(1000.0),
                iteration_count: 1.0,
                ..VisualAnimation::default().timing
            },
            keyframes: vec![keyframe(0.0, rotate(0.0)), keyframe(1.0, rotate(1.0))],
            ..VisualAnimation::default()
        };
        let opacity = VisualAnimation {
            target_kind: FfiVisualAnimationTargetKind::Opacity,
            node_indices: vec![effect.0],
            timing: AnimationEffectTiming {
                iteration_duration: TimeValue::milliseconds(1000.0),
                ..VisualAnimation::default().timing
            },
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
