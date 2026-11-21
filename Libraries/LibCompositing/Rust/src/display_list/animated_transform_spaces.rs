/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The raster spaces at spatial nodes whose transform the compositor animates: which nodes get one, the scale their
//! content is rastered at, and where that content can show around the node wherever the animation takes it.

use crate::display_list::commands::{EffectNodeIndex, SpatialNodeIndex, VISUAL_VIEWPORT_NODE_INDEX};
use crate::display_list::placement::ValuesFoldedDownSpatialChains;
use crate::easing::Easing;
use crate::host::{FfiVisualAnimationTargetKind, FfiVisualAnimationTransformOperationKind};
use crate::visual_animation::{VisualAnimation, VisualAnimationTransformOperation, VisualAnimationValue};
use crate::visual_context::{EffectNodeData, SpatialData, VisualContextTree};
use libgfx_rust::{FloatMatrix4x4, FloatRect};

pub const MAXIMUM_ANIMATED_TRANSFORM_SPACE_COUNT: usize = 32;
const LARGEST_RASTER_SCALE_PAST_PARENT_SCALE: f32 = 4.0;
pub(crate) const SMALLEST_RASTER_SCALE: f32 = 1.0 / 64.0;
const RASTER_SCALE_STEPS_PER_UNIT: f32 = 64.0;

fn transform_animations_of(tree: &VisualContextTree) -> impl Iterator<Item = &VisualAnimation> {
    tree.visual_animations()
        .iter()
        .filter(|animation| animation.target_kind == FfiVisualAnimationTargetKind::Transform)
}

/// The nodes whose transform the compositor animates that `is_root` takes.
pub(crate) fn roots_of_transform_animations(
    tree: &VisualContextTree,
    is_root: impl Fn(SpatialNodeIndex) -> bool,
) -> impl Iterator<Item = SpatialNodeIndex> {
    transform_animations_of(tree)
        .flat_map(|animation| animation.node_indices.iter().map(|&index| SpatialNodeIndex(index)))
        .filter(move |&node| is_root(node))
}

/// Per spatial node, whether it is or is under a node whose transform the compositor animates that `is_root` takes, or
/// empty where it takes none.
pub(crate) fn spatial_nodes_under_transform_animations(
    tree: &VisualContextTree,
    is_root: impl Fn(SpatialNodeIndex) -> bool,
) -> Vec<bool> {
    let roots: Vec<SpatialNodeIndex> = roots_of_transform_animations(tree, is_root).collect();
    if roots.is_empty() {
        return Vec::new();
    }
    tree.spatial_nodes_in_subtrees_of(&roots)
}

fn matrix_of_operations(operations: &[VisualAnimationTransformOperation]) -> FloatMatrix4x4 {
    operations.iter().fold(FloatMatrix4x4::identity(), |matrix, operation| {
        matrix.multiplied(operation.to_matrix())
    })
}

fn transform_keyframe_matrices(animation: &VisualAnimation) -> impl Iterator<Item = FloatMatrix4x4> + '_ {
    animation.keyframes.iter().filter_map(|keyframe| match &keyframe.value {
        VisualAnimationValue::Transform(operations) => Some(matrix_of_operations(operations)),
        _ => None,
    })
}

/// Whether every sample of a transform animation keeps what it transforms in the plane, so that its samples are 2D
/// affine. A sample interpolates the values of each operation of two keyframes, so each operation has to stay in the
/// plane at every keyframe, and a rotation out of the plane has to keep its angle, as one between two angles that stay
/// in the plane can leave it.
pub fn transform_animation_stays_in_plane(animation: &VisualAnimation) -> bool {
    use FfiVisualAnimationTransformOperationKind as Kind;
    let Some(operations_of_keyframes) = animation
        .keyframes
        .iter()
        .map(|keyframe| match &keyframe.value {
            VisualAnimationValue::Transform(operations) => Some(operations.as_slice()),
            _ => None,
        })
        .collect::<Option<Vec<_>>>()
    else {
        return false;
    };
    let each_operation_stays_in_plane = operations_of_keyframes
        .iter()
        .flat_map(|operations| operations.iter())
        .all(|operation| operation.to_matrix().is_2d_affine());
    let each_rotation_out_of_plane_keeps_its_angle = operations_of_keyframes.windows(2).all(|pair| {
        pair[0]
            .iter()
            .zip(pair[1])
            .all(|(from, to)| !matches!(from.kind, Kind::RotateX | Kind::RotateY) || from.values() == to.values())
    });
    each_operation_stays_in_plane && each_rotation_out_of_plane_keeps_its_angle
}

/// The largest factor a 2D affine matrix scales lengths along either axis by.
fn largest_axis_scale(matrix: FloatMatrix4x4) -> f32 {
    let affine = matrix.extract_2d_affine();
    let x_axis_scale = (affine.a() * affine.a() + affine.b() * affine.b()).sqrt();
    let y_axis_scale = (affine.c() * affine.c() + affine.d() * affine.d()).sqrt();
    x_axis_scale.max(y_axis_scale)
}

fn matrix_from_root_to_node_space(tree: &VisualContextTree, node: SpatialNodeIndex) -> FloatMatrix4x4 {
    let mut chain = Vec::new();
    let mut index = node;
    loop {
        chain.push(index);
        if index == VISUAL_VIEWPORT_NODE_INDEX {
            break;
        }
        index = tree.spatial_nodes[index.0 as usize].parent;
    }
    chain.iter().rev().fold(FloatMatrix4x4::identity(), |matrix, &index| {
        tree.local_transform(index, &[]).applied_to(matrix)
    })
}

/// The scale content under `anchor` is rastered at: as far as its parent scales it times the largest scale any of its
/// keyframes reach, so that the content stays as sharp as a direct raster at the largest, rounded so that samples
/// of one animation share it.
pub(crate) fn raster_scale_of(tree: &VisualContextTree, anchor: SpatialNodeIndex) -> f32 {
    let parent_scale = largest_axis_scale(matrix_from_root_to_node_space(
        tree,
        tree.spatial_nodes[anchor.0 as usize].parent,
    ));
    let largest_keyframe_scale = transform_animations_of(tree)
        .filter(|animation| animation.node_indices.contains(&anchor.0))
        .flat_map(transform_keyframe_matrices)
        .map(largest_axis_scale)
        .fold(0.0f32, f32::max);
    let scale_at_largest_keyframe = parent_scale * largest_keyframe_scale;
    if !parent_scale.is_finite() || scale_at_largest_keyframe.is_nan() {
        return 1.0;
    }
    // A parent that scales content down far enough puts the largest scale below the smallest one, which then wins.
    let largest_raster_scale = (parent_scale * LARGEST_RASTER_SCALE_PAST_PARENT_SCALE).max(SMALLEST_RASTER_SCALE);
    let raster_scale = scale_at_largest_keyframe
        .max(SMALLEST_RASTER_SCALE)
        .min(largest_raster_scale);
    if !raster_scale.is_finite() {
        return 1.0;
    }
    ((raster_scale * RASTER_SCALE_STEPS_PER_UNIT).round() / RASTER_SCALE_STEPS_PER_UNIT).max(SMALLEST_RASTER_SCALE)
}

fn effect_reads_what_is_below_it(data: &EffectNodeData) -> bool {
    match data {
        EffectNodeData::Effects(effects) => {
            effects.backdrop_filter.is_some()
                || effects.blend_mode != libgfx_rust::CompositingAndBlendingOperator::Normal
        }
        EffectNodeData::Mask(mask) => mask.masks_backdrop,
        _ => false,
    }
}

/// The spatial nodes whose transform the compositor animates that get raster spaces of their own, as the tree alone
/// decides: the outermost of them, whose animations stay in the plane, under 2D affine
/// transforms, holding no effect that reads what is below it and no part of a 3D sorting context, in tree order up
/// to as many as get one.
pub fn animated_transform_anchors(tree: &VisualContextTree) -> Vec<SpatialNodeIndex> {
    let mut is_animated_by_spatial_index = vec![false; tree.spatial_nodes.len()];
    let mut stays_in_plane_by_spatial_index = vec![true; tree.spatial_nodes.len()];
    for animation in transform_animations_of(tree) {
        let stays_in_plane = transform_animation_stays_in_plane(animation);
        for &node_index in &animation.node_indices {
            let Some(is_animated) = is_animated_by_spatial_index.get_mut(node_index as usize) else {
                continue;
            };
            *is_animated = true;
            stays_in_plane_by_spatial_index[node_index as usize] &= stays_in_plane;
        }
    }
    if !is_animated_by_spatial_index.contains(&true) {
        return Vec::new();
    }
    let mut chain_below_viewport_holds_animated_node = ValuesFoldedDownSpatialChains::<bool>::new();
    let mut matrices_from_root_to_node_spaces = ValuesFoldedDownSpatialChains::<FloatMatrix4x4>::new();
    let matrix_from_root_to_viewport_node_space = tree
        .local_transform(VISUAL_VIEWPORT_NODE_INDEX, &[])
        .applied_to(FloatMatrix4x4::identity());
    let mut subtrees_that_keep_nodes_from_anchoring_spaces = None;
    let mut anchors = Vec::new();
    for (index, node) in tree.spatial_nodes.iter().enumerate() {
        if anchors.len() == MAXIMUM_ANIMATED_TRANSFORM_SPACE_COUNT {
            break;
        }
        let anchor = SpatialNodeIndex(index as u32);
        if !is_animated_by_spatial_index[index]
            || !stays_in_plane_by_spatial_index[index]
            || !matches!(node.data, SpatialData::Transform(_))
        {
            continue;
        }
        let is_outermost = node.parent == VISUAL_VIEWPORT_NODE_INDEX
            || chain_below_viewport_holds_animated_node.value_of(
                tree,
                node.parent,
                false,
                |node, chain_above_holds| chain_above_holds || is_animated_by_spatial_index[node.0 as usize],
            ) == Some(false);
        if !is_outermost {
            continue;
        }
        let is_under_2d_affine_transforms = matrices_from_root_to_node_spaces
            .value_of(
                tree,
                node.parent,
                matrix_from_root_to_viewport_node_space,
                |node, matrix_from_root_to_parent_space| {
                    tree.local_transform(node, &[])
                        .applied_to(matrix_from_root_to_parent_space)
                },
            )
            .is_some_and(|matrix| matrix.is_2d_affine());
        if !is_under_2d_affine_transforms {
            continue;
        }
        let subtrees = subtrees_that_keep_nodes_from_anchoring_spaces
            .get_or_insert_with(|| SubtreesThatKeepNodesFromAnchoringSpaces::of(tree));
        if subtrees.touch_sorting_context_by_spatial_index[index]
            || subtrees.hold_effect_reading_below_it_by_spatial_index[index]
        {
            continue;
        }
        anchors.push(anchor);
    }
    anchors
}

// Per spatial node, whether its subtree holds part of a 3D sorting context, and whether it holds an effect that reads
// what is below it.
struct SubtreesThatKeepNodesFromAnchoringSpaces {
    touch_sorting_context_by_spatial_index: Vec<bool>,
    hold_effect_reading_below_it_by_spatial_index: Vec<bool>,
}

impl SubtreesThatKeepNodesFromAnchoringSpaces {
    fn of(tree: &VisualContextTree) -> Self {
        let mut touch_sorting_context_by_spatial_index: Vec<bool> = tree
            .roots_of_contexts_sorting_planes_by_depth()
            .iter()
            .map(Option::is_some)
            .collect();
        let mut hold_effect_reading_below_it_by_spatial_index = vec![false; tree.spatial_nodes.len()];
        for effect_node in &tree.effect_nodes {
            if effect_reads_what_is_below_it(&effect_node.data)
                && let Some(holds) =
                    hold_effect_reading_below_it_by_spatial_index.get_mut(effect_node.spatial.0 as usize)
            {
                *holds = true;
            }
        }
        let mut live_nodes_parents_first = Vec::with_capacity(tree.spatial_nodes.len());
        tree.visit_spatial_nodes_parents_first(|index| live_nodes_parents_first.push(index));
        for &index in live_nodes_parents_first.iter().rev() {
            if index == VISUAL_VIEWPORT_NODE_INDEX.0 as usize {
                continue;
            }
            let parent = tree.spatial_nodes[index].parent.0 as usize;
            touch_sorting_context_by_spatial_index[parent] |= touch_sorting_context_by_spatial_index[index];
            hold_effect_reading_below_it_by_spatial_index[parent] |=
                hold_effect_reading_below_it_by_spatial_index[index];
        }
        Self {
            touch_sorting_context_by_spatial_index,
            hold_effect_reading_below_it_by_spatial_index,
        }
    }
}

/// Whether an effect reads what is below it, which content of a space whose anchor the compositor animates cannot be
/// placed under.
pub fn effect_reads_below_it(tree: &VisualContextTree, effect: EffectNodeIndex) -> bool {
    effect_reads_what_is_below_it(&tree.effect_nodes[effect.0 as usize].data)
}

pub(crate) fn easing_stays_within_unit_interval(easing: &Easing) -> bool {
    match easing {
        Easing::Linear(points) => points.iter().all(|point| (0.0..=1.0).contains(&point.output)),
        Easing::CubicBezier { y1, y2, .. } => (0.0..=1.0).contains(y1) && (0.0..=1.0).contains(y2),
        Easing::Steps { .. } => true,
    }
}

#[derive(Clone, Copy)]
struct ValueRange {
    lowest: f32,
    highest: f32,
}

impl ValueRange {
    fn of(value: f32) -> Self {
        Self {
            lowest: value,
            highest: value,
        }
    }

    fn widened_to(self, value: f32) -> Self {
        Self {
            lowest: self.lowest.min(value),
            highest: self.highest.max(value),
        }
    }

    fn times(self, lowest_factor: f32, highest_factor: f32) -> Self {
        let products = [
            self.lowest * lowest_factor,
            self.lowest * highest_factor,
            self.highest * lowest_factor,
            self.highest * highest_factor,
        ];
        Self {
            lowest: products.iter().copied().fold(f32::INFINITY, f32::min),
            highest: products.iter().copied().fold(f32::NEG_INFINITY, f32::max),
        }
    }
}

/// What a 2D operation can do to points, over the values its keyframes give it.
#[derive(Clone, Copy)]
enum SweptOperation {
    Translate { x: ValueRange, y: ValueRange },
    Scale { x: ValueRange, y: ValueRange },
    Rotate,
}

fn x_and_y_of_operation(operation: &VisualAnimationTransformOperation) -> Option<(f32, f32)> {
    use FfiVisualAnimationTransformOperationKind as Kind;
    let values = operation.values();
    let first = values.first().copied().unwrap_or(0.0);
    let second = values.get(1).copied();
    Some(match operation.kind {
        Kind::Translate => (first, second.unwrap_or(0.0)),
        Kind::TranslateX => (first, 0.0),
        Kind::TranslateY => (0.0, first),
        Kind::Scale => (first, second.unwrap_or(first)),
        Kind::ScaleX => (first, 1.0),
        Kind::ScaleY => (1.0, first),
        Kind::Rotate | Kind::RotateZ => (0.0, 0.0),
        _ => return None,
    })
}

fn swept_operations_of(animation: &VisualAnimation) -> Option<Vec<SweptOperation>> {
    use FfiVisualAnimationTransformOperationKind as Kind;
    if !easing_stays_within_unit_interval(&animation.easing)
        || !animation
            .keyframes
            .iter()
            .all(|keyframe| easing_stays_within_unit_interval(&keyframe.easing))
    {
        return None;
    }
    let mut swept_operations: Option<Vec<SweptOperation>> = None;
    for keyframe in &animation.keyframes {
        let VisualAnimationValue::Transform(operations) = &keyframe.value else {
            return None;
        };
        let operations_of_keyframe = operations
            .iter()
            .map(|operation| {
                let (x, y) = x_and_y_of_operation(operation)?;
                Some(match operation.kind {
                    Kind::Translate | Kind::TranslateX | Kind::TranslateY => SweptOperation::Translate {
                        x: ValueRange::of(x),
                        y: ValueRange::of(y),
                    },
                    Kind::Scale | Kind::ScaleX | Kind::ScaleY => SweptOperation::Scale {
                        x: ValueRange::of(x),
                        y: ValueRange::of(y),
                    },
                    _ => SweptOperation::Rotate,
                })
            })
            .collect::<Option<Vec<_>>>()?;
        swept_operations = Some(match swept_operations {
            None => operations_of_keyframe,
            Some(swept_so_far) => {
                if swept_so_far.len() != operations_of_keyframe.len() {
                    return None;
                }
                swept_so_far
                    .into_iter()
                    .zip(operations_of_keyframe)
                    .map(|pair| match pair {
                        (
                            SweptOperation::Translate { x, y },
                            SweptOperation::Translate {
                                x: keyframe_x,
                                y: keyframe_y,
                            },
                        ) => Some(SweptOperation::Translate {
                            x: x.widened_to(keyframe_x.lowest),
                            y: y.widened_to(keyframe_y.lowest),
                        }),
                        (
                            SweptOperation::Scale { x, y },
                            SweptOperation::Scale {
                                x: keyframe_x,
                                y: keyframe_y,
                            },
                        ) => Some(SweptOperation::Scale {
                            x: x.widened_to(keyframe_x.lowest),
                            y: y.widened_to(keyframe_y.lowest),
                        }),
                        (SweptOperation::Rotate, SweptOperation::Rotate) => Some(SweptOperation::Rotate),
                        _ => None,
                    })
                    .collect::<Option<Vec<_>>>()?
            }
        });
    }
    swept_operations
}

fn swept_through(operation: SweptOperation, rect: FloatRect) -> FloatRect {
    let (x, y) = (
        ValueRange {
            lowest: rect.x,
            highest: rect.right(),
        },
        ValueRange {
            lowest: rect.y,
            highest: rect.bottom(),
        },
    );
    let (x, y) = match operation {
        SweptOperation::Translate {
            x: translation_x,
            y: translation_y,
        } => (
            ValueRange {
                lowest: x.lowest + translation_x.lowest,
                highest: x.highest + translation_x.highest,
            },
            ValueRange {
                lowest: y.lowest + translation_y.lowest,
                highest: y.highest + translation_y.highest,
            },
        ),
        SweptOperation::Scale { x: scale_x, y: scale_y } => (
            x.times(scale_x.lowest, scale_x.highest),
            y.times(scale_y.lowest, scale_y.highest),
        ),
        SweptOperation::Rotate => {
            let radius = [
                (x.lowest, y.lowest),
                (x.highest, y.lowest),
                (x.lowest, y.highest),
                (x.highest, y.highest),
            ]
            .iter()
            .map(|&(corner_x, corner_y)| (corner_x * corner_x + corner_y * corner_y).sqrt())
            .fold(0.0f32, f32::max);
            (
                ValueRange {
                    lowest: -radius,
                    highest: radius,
                },
                ValueRange {
                    lowest: -radius,
                    highest: radius,
                },
            )
        }
    };
    FloatRect::new(x.lowest, y.lowest, x.highest - x.lowest, y.highest - y.lowest)
}

/// Where content within `content_rect`, in the coordinates of `anchor`'s content, can show in those of the anchor's
/// parent wherever the compositor's animation of the anchor takes it, or what the tree holds while the animation shows
/// nothing; none where that cannot be bounded, as for a skew or an easing that overshoots.
pub fn swept_bounds_in_parent(
    tree: &VisualContextTree,
    anchor: SpatialNodeIndex,
    content_rect: FloatRect,
) -> Option<FloatRect> {
    let SpatialData::Transform(transform) = &tree.spatial_nodes[anchor.0 as usize].data else {
        return None;
    };
    let mut animations_of_anchor =
        transform_animations_of(tree).filter(|animation| animation.node_indices.contains(&anchor.0));
    let animation = animations_of_anchor.next()?;
    if animations_of_anchor.next().is_some() {
        return None;
    }
    let origin = transform.origin;
    let mut swept = content_rect.translated(-origin.x, -origin.y);
    for operation in swept_operations_of(animation)?.into_iter().rev() {
        swept = swept_through(operation, swept);
    }
    let swept = swept.translated(origin.x, origin.y);
    let where_tree_holds_it = transform
        .matrix_including_origin()
        .extract_2d_affine()
        .map_rect(content_rect);
    let bounds = swept.united(where_tree_holds_it);
    crate::display_list::placement::is_finite_rect(bounds).then_some(bounds)
}
