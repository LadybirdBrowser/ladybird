/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::{
    ClipNode, ClipNodeData, ClipNodeIndex, ClipPathData, CompositorScrollTable, EffectNode, EffectNodeData,
    EffectNodeIndex, SpatialData, SpatialNode, SpatialNodeIndex, VISUAL_VIEWPORT_NODE_INDEX, VisualContextTree,
};
use crate::host::FfiVisualAnimationTransformOperationKind;
use crate::visual_animation::{
    MAXIMUM_TRANSFORM_OPERATION_VALUE_COUNT, VisualAnimation, VisualAnimationTransformOperation,
};
use libgfx_rust::filter::Filter;
use libgfx_rust::path::Path;
use libipc_rust::wire::{Decode, DecodeError, Decoder, Encode, EncodeError, Encoder};

fn decode_nodes_reserving_their_count<T: Decode>(
    decoder: &mut Decoder<'_>,
    node_count: usize,
) -> Result<Vec<T>, DecodeError> {
    if node_count > decoder.remaining_bytes() / T::MIN_ENCODED_SIZE {
        return Err(DecodeError::UnexpectedEnd);
    }
    let mut nodes = Vec::new();
    nodes
        .try_reserve_exact(node_count)
        .map_err(|_| DecodeError::AllocationFailed)?;
    for _ in 0..node_count {
        nodes.push(decoder.decode()?);
    }
    Ok(nodes)
}

impl Encode for SpatialNode {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.parent)?;
        encoder.encode(&self.data)
    }
}

impl Decode for SpatialNode {
    const MIN_ENCODED_SIZE: usize = SpatialNodeIndex::MIN_ENCODED_SIZE + SpatialData::MIN_ENCODED_SIZE;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        let parent = decoder.decode()?;
        let data = decoder.decode()?;
        Ok(Self { data, parent })
    }

    fn decode_vec(decoder: &mut Decoder<'_>, size: usize) -> Result<Vec<Self>, DecodeError> {
        decode_nodes_reserving_their_count(decoder, size)
    }
}

impl Encode for ClipPathData {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.bounding_rect)?;
        encoder.encode(&self.fill_rule)?;
        encoder.encode(self.path.serialize_to_bytes().as_slice())
    }
}

impl Decode for ClipPathData {
    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        let bounding_rect = decoder.decode()?;
        let fill_rule = decoder.decode()?;
        let serialized_path_size = decoder.decode_size()?;
        let path = Path::from_serialized_bytes(decoder.read_bytes(serialized_path_size)?);
        Ok(Self {
            path: std::sync::Arc::new(path),
            bounding_rect,
            fill_rule,
        })
    }
}

impl Encode for ClipNode {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.parent)?;
        encoder.encode(&self.spatial)?;
        encoder.encode(&self.data)
    }
}

impl Decode for ClipNode {
    const MIN_ENCODED_SIZE: usize =
        ClipNodeIndex::MIN_ENCODED_SIZE + SpatialNodeIndex::MIN_ENCODED_SIZE + ClipNodeData::MIN_ENCODED_SIZE;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        let parent = decoder.decode()?;
        let spatial = decoder.decode()?;
        let data = decoder.decode()?;
        Ok(Self::new(data, parent, spatial))
    }

    fn decode_vec(decoder: &mut Decoder<'_>, size: usize) -> Result<Vec<Self>, DecodeError> {
        decode_nodes_reserving_their_count(decoder, size)
    }
}

impl Encode for EffectNode {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.parent)?;
        encoder.encode(&self.spatial)?;
        encoder.encode(&self.local_clip)?;
        encoder.encode(&self.data)
    }
}

impl Decode for EffectNode {
    const MIN_ENCODED_SIZE: usize = EffectNodeIndex::MIN_ENCODED_SIZE
        + SpatialNodeIndex::MIN_ENCODED_SIZE
        + ClipNodeIndex::MIN_ENCODED_SIZE
        + EffectNodeData::MIN_ENCODED_SIZE;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self {
            parent: decoder.decode()?,
            spatial: decoder.decode()?,
            local_clip: decoder.decode()?,
            data: decoder.decode()?,
        })
    }

    fn decode_vec(decoder: &mut Decoder<'_>, size: usize) -> Result<Vec<Self>, DecodeError> {
        decode_nodes_reserving_their_count(decoder, size)
    }
}

impl Encode for VisualAnimationTransformOperation {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.kind)?;
        encoder.encode(&(self.values().len() as u8))?;
        f32::encode_slice(self.values(), encoder)
    }
}

impl Decode for VisualAnimationTransformOperation {
    const MIN_ENCODED_SIZE: usize = FfiVisualAnimationTransformOperationKind::MIN_ENCODED_SIZE + size_of::<u8>();

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        let kind = decoder.decode()?;
        let value_count = usize::from(decoder.decode::<u8>()?);
        if value_count > MAXIMUM_TRANSFORM_OPERATION_VALUE_COUNT {
            return Err(DecodeError::InvalidValue("VisualAnimationTransformOperation"));
        }
        let mut values = [0.0; MAXIMUM_TRANSFORM_OPERATION_VALUE_COUNT];
        for value in &mut values[..value_count] {
            *value = decoder.decode()?;
        }
        Self::new(kind, &values[..value_count]).ok_or(DecodeError::InvalidValue("VisualAnimationTransformOperation"))
    }
}

fn spatial_node_has_ancestor(nodes: &[SpatialNode], mut node: usize, ancestor: SpatialNodeIndex) -> bool {
    while node != VISUAL_VIEWPORT_NODE_INDEX.0 as usize {
        node = nodes[node].parent.0 as usize;
        if node == ancestor.0 as usize {
            return true;
        }
    }
    false
}

impl VisualContextTree {
    pub fn node_references_are_consistent(&self) -> bool {
        let spatial_nodes = &self.spatial_nodes;
        let root = &spatial_nodes[VISUAL_VIEWPORT_NODE_INDEX.0 as usize];
        if root.parent != VISUAL_VIEWPORT_NODE_INDEX || !matches!(root.data, SpatialData::Transform(_)) {
            return false;
        }
        let spatial_order = self.spatial_dependency_order_with_back_edges();
        if !spatial_order.back_edges.is_empty() || !spatial_order.dangling_references.is_empty() {
            return false;
        }
        for (index, node) in spatial_nodes.iter().enumerate() {
            match &node.data {
                // The hit-test walk looks the plane root up on the marker's root path, so a node on
                // another branch must be rejected here rather than fail that lookup.
                SpatialData::BackfaceVisibility(backface) => {
                    if !spatial_node_has_ancestor(spatial_nodes, index, backface.plane_root_index) {
                        return false;
                    }
                }
                // resolve_sticky_offsets() reads the referenced nodes by kind, so a hostile tree must not
                // get past this point with references of the wrong kind.
                SpatialData::Sticky(sticky) => {
                    let scroller_is_valid = sticky.scroller == VISUAL_VIEWPORT_NODE_INDEX
                        || matches!(spatial_nodes[sticky.scroller.0 as usize].data, SpatialData::Scroll(_));
                    let parent_sticky_is_valid = sticky
                        .parent_sticky
                        .is_none_or(|parent| matches!(spatial_nodes[parent.0 as usize].data, SpatialData::Sticky(_)));
                    if !scroller_is_valid || !parent_sticky_is_valid {
                        return false;
                    }
                }
                _ => {}
            }
        }
        // A node's spatial node is in range, and live where the node is live.
        let spatial_reference_is_consistent = |spatial: SpatialNodeIndex, is_live: bool| {
            (spatial.0 as usize) < spatial_nodes.len() && (!is_live || self.spatial_is_live(spatial))
        };
        if !self
            .clip_nodes
            .iter()
            .all(|node| spatial_reference_is_consistent(node.spatial, node.data.is_live()))
        {
            return false;
        }
        let clip_order = self.clip_dependency_order_with_back_edges();
        if !clip_order.back_edges.is_empty() || !clip_order.dangling_references.is_empty() {
            return false;
        }
        // Starting clips are local facts and may cross along the effect chain. The
        // display list's replay plan derives nested output clips from its actual runs.
        for node in &self.effect_nodes {
            if !spatial_reference_is_consistent(node.spatial, node.data.is_live()) {
                return false;
            }
            if node.data.is_live() && !self.clip_is_none_or_live(node.local_clip) {
                return false;
            }
        }
        let effect_order = self.effect_dependency_order_with_back_edges();
        if !effect_order.back_edges.is_empty() || !effect_order.dangling_references.is_empty() {
            return false;
        }
        // Plane clips are pushed immediately above this root isolation layer.
        if let Some(effect) = self.root_isolation_effect {
            let Some(node) = self.effect_nodes.get(effect.0 as usize) else {
                return false;
            };
            if !matches!(node.data, EffectNodeData::Effects(_)) || !node.parent.is_none() || !node.local_clip.is_none()
            {
                return false;
            }
        }
        true
    }
}

impl Encode for VisualContextTree {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.structural_epoch)?;
        encoder.encode(&self.root_isolation_effect)?;
        encoder.encode(&self.spatial_nodes)?;
        encoder.encode(&self.clip_nodes)?;
        encoder.encode(&self.effect_nodes)?;
        encoder.encode(&self.visual_animations)?;
        encoder.encode(&self.compositor_scroll_table)
    }
}

impl Decode for VisualContextTree {
    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        let structural_epoch = decoder.decode()?;
        let root_isolation_effect = decoder.decode()?;
        let spatial_nodes = decoder.decode::<Vec<SpatialNode>>()?;
        if spatial_nodes.is_empty() {
            return Err(DecodeError::InvalidValue("VisualContextTree without a root"));
        }
        let clip_nodes = decoder.decode()?;
        let effect_nodes = decoder.decode()?;
        let visual_animations = decoder.decode::<Vec<VisualAnimation>>()?;
        let compositor_scroll_table = decoder.decode::<CompositorScrollTable>()?;

        let mut tree = Self::from_nodes(
            spatial_nodes,
            clip_nodes,
            effect_nodes,
            root_isolation_effect,
            structural_epoch,
        );
        if !tree.node_references_are_consistent() {
            return Err(DecodeError::InvalidValue("VisualContextTree node references"));
        }
        // The player builds the filters of the effect nodes from their bytes without checking them.
        let filter_is_well_formed = |bytes: &Vec<u8>| Filter::deserialize(bytes).is_some();
        if !tree.effect_nodes.iter().all(|node| match &node.data {
            EffectNodeData::Effects(effects) => {
                effects.filter.as_deref().is_none_or(filter_is_well_formed)
                    && effects
                        .backdrop_filter
                        .as_ref()
                        .is_none_or(|backdrop| filter_is_well_formed(&backdrop.filter))
            }
            _ => true,
        }) {
            return Err(DecodeError::InvalidValue("VisualContextTree filters"));
        }
        // The bytes come from another process: an animation must be one the tree can sample, and
        // must name nodes of the kind it drives.
        if !visual_animations.iter().all(|animation| {
            animation.is_valid()
                && tree.visual_animation_targets_are_valid(animation.target_kind, &animation.node_indices)
        }) {
            return Err(DecodeError::InvalidValue("VisualContextTree animations"));
        }
        if !compositor_scroll_table.is_valid_for_tree(&tree) {
            return Err(DecodeError::InvalidValue("VisualContextTree compositor scroll table"));
        }
        tree.set_compositor_scroll_table(compositor_scroll_table);
        tree.set_visual_animations(visual_animations);
        Ok(tree)
    }
}

#[cfg(test)]
impl VisualContextTree {
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut encoder = Encoder::new();
        encoder.encode(self).expect("a visual context tree encodes");
        encoder.finish().0
    }

    pub fn from_bytes(bytes: &[u8]) -> Result<Self, DecodeError> {
        let mut decoder = Decoder::new(bytes, []);
        let tree = decoder.decode()?;
        if !decoder.is_at_end() {
            return Err(DecodeError::InvalidValue("bytes after a VisualContextTree"));
        }
        Ok(tree)
    }
}

#[cfg(test)]
mod tests {
    use super::super::{
        AnchorScrollShift, BackdropFilterData, BackfaceVisibilityData, ClipData, ClipMode, CompositorScrollNodeKind,
        CompositorScrollNodeRecord, CompositorSnapAreaRecord, CompositorSnapContainerRecord, EffectsData, MaskData,
        MaskLayerOrigin, PerspectiveData, ScrollData, StickyData, TransformData, TransformDataRole,
        scroll_state::NO_SCROLL_STATE_SLOT,
    };
    use super::*;
    use crate::animation_timing::{AnimationEffectTiming, FillMode, PlaybackDirection, TimeValue};
    use crate::css_pixels::{CssPixels, FfiCssPixelPoint, FfiCssPixelRect};
    use crate::display_list::commands::UniqueNodeId;
    use crate::easing::{Easing, FfiLinearEasingPoint};
    use crate::filter_bytes::{FfiFilterFunction, FfiFilterFunctionKind};
    use crate::host::FfiVisualAnimationTargetKind;
    use crate::node_slot_id::NodeSlotId;
    use crate::visual_animation::{VisualAnimationKeyframe, VisualAnimationValue};
    use libgfx_rust::{
        Color, ColorFilterType, CompositingAndBlendingOperator, CornerRadii, CornerRadius, FloatMatrix4x4, FloatPoint,
        FloatRect, FloatSize, IntRect, MaskKind, WindingRule, perspective_matrix, translation_matrix,
    };

    fn transform(matrix: FloatMatrix4x4) -> TransformData {
        TransformData {
            matrix,
            origin: FloatPoint { x: 1.0, y: 2.0 },
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
            bounds: FloatRect::default(),
        }
    }

    fn tree_with_every_node_kind() -> VisualContextTree {
        let mut tree = VisualContextTree::create(transform(FloatMatrix4x4::identity()));
        tree.structural_epoch = 42;
        let scroll_node = tree.append_spatial(
            SpatialData::Scroll(ScrollData {
                state_slot: NO_SCROLL_STATE_SLOT,
                owner_paintable: NodeSlotId::INVALID,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let sorting_root = tree.append_spatial(
            SpatialData::Transform(TransformData {
                sorting_context_root_index: Some(VISUAL_VIEWPORT_NODE_INDEX),
                flattens_inherited_transform: true,
                role: TransformDataRole::SvgViewportTransform,
                synthetic_plane: true,
                establishes_sorting_context: false,
                ..transform(translation_matrix(3.0, 4.0, 5.0))
            }),
            scroll_node,
        );
        let outer_sticky = tree.append_spatial(
            SpatialData::Sticky(StickyData {
                scroller: scroll_node,
                parent_sticky: None,
                position_relative_to_scroller: FloatPoint { x: 1.0, y: 100.0 },
                border_box_origin: FloatPoint::default(),
                border_box_size: FloatSize {
                    width: 800.0,
                    height: 50.0,
                },
                scrollport_size: FloatSize {
                    width: 800.0,
                    height: 600.0,
                },
                containing_block_region: FloatRect::new(0.0, 0.0, 800.0, 2000.0),
                needs_parent_offset_adjustment: true,
                non_scrolling_scroll_container: NodeSlotId::INVALID,
                inset_top: Some(0.0),
                inset_right: None,
                inset_bottom: Some(2.5),
                inset_left: None,
                box_node_id: UniqueNodeId(41),
                box_pseudo_element_type: 2,
                state_slot: NO_SCROLL_STATE_SLOT,
                owner_paintable: NodeSlotId::INVALID,
            }),
            scroll_node,
        );
        tree.append_spatial(
            SpatialData::Sticky(StickyData {
                scroller: scroll_node,
                parent_sticky: Some(outer_sticky),
                position_relative_to_scroller: FloatPoint { x: 0.0, y: 110.0 },
                border_box_origin: FloatPoint::default(),
                border_box_size: FloatSize {
                    width: 800.0,
                    height: 10.0,
                },
                scrollport_size: FloatSize {
                    width: 800.0,
                    height: 600.0,
                },
                containing_block_region: FloatRect::new(0.0, 100.0, 800.0, 50.0),
                needs_parent_offset_adjustment: false,
                non_scrolling_scroll_container: NodeSlotId { index: 7 },
                inset_top: None,
                inset_right: Some(1.0),
                inset_bottom: None,
                inset_left: Some(3.0),
                box_node_id: UniqueNodeId(0),
                box_pseudo_element_type: 0,
                state_slot: NO_SCROLL_STATE_SLOT,
                owner_paintable: NodeSlotId::INVALID,
            }),
            outer_sticky,
        );
        let perspective = tree.append_spatial(
            SpatialData::Perspective(PerspectiveData {
                matrix: perspective_matrix(1000.0),
                flattens_inherited_transform: true,
            }),
            sorting_root,
        );
        tree.append_spatial(
            SpatialData::BackfaceVisibility(BackfaceVisibilityData {
                plane_root_index: sorting_root,
                flattens_inherited_transform: false,
            }),
            perspective,
        );
        tree.append_spatial(
            SpatialData::AnchorScrollShift(AnchorScrollShift {
                scroll_node_index: scroll_node,
                negate: true,
                compensate_horizontal_scroll: false,
                compensate_vertical_scroll: true,
            }),
            scroll_node,
        );

        let clip = tree.append_clip(
            ClipNodeData::Rect(ClipData {
                rect: FloatRect::new(0.5, 1.0, 20.0, 30.0),
                corner_radii: CornerRadii {
                    top_left: CornerRadius {
                        horizontal_radius: 1,
                        vertical_radius: 2,
                    },
                    top_right: CornerRadius {
                        horizontal_radius: 3,
                        vertical_radius: 4,
                    },
                    bottom_right: CornerRadius {
                        horizontal_radius: 5,
                        vertical_radius: 6,
                    },
                    bottom_left: CornerRadius {
                        horizontal_radius: 7,
                        vertical_radius: 8,
                    },
                },
                mode: ClipMode::Difference,
            }),
            ClipNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let clip_path = tree.append_clip(
            ClipNodeData::Path(ClipPathData {
                path: std::sync::Arc::new(Path::from_serialized_bytes(&[])),
                bounding_rect: IntRect::new(5, 6, 7, 8),
                fill_rule: WindingRule::EvenOdd,
            }),
            clip,
            scroll_node,
        );
        let root_effect = tree.append_effect(
            EffectNodeData::Effects(EffectsData {
                isolation: true,
                opacity: 1.0,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
                backdrop_filter: None,
            }),
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );
        let effects = tree.append_effect(
            EffectNodeData::Effects(EffectsData {
                isolation: false,
                opacity: 0.25,
                blend_mode: CompositingAndBlendingOperator::PlusLighter,
                filter: Some(std::sync::Arc::new(Filter::hue_rotate(90.0, None).serialize())),
                backdrop_filter: Some(BackdropFilterData {
                    filter: std::sync::Arc::new(Filter::blur(2.0, 2.0, None).serialize()),
                    region: IntRect::new(9, 10, 11, 12),
                    corner_radii: CornerRadii {
                        top_left: CornerRadius {
                            horizontal_radius: 1,
                            vertical_radius: 2,
                        },
                        ..CornerRadii::default()
                    },
                }),
            }),
            root_effect,
            scroll_node,
            clip,
        );
        let mask = tree.append_effect(
            EffectNodeData::Mask(MaskData {
                rect: IntRect::new(1, 2, 3, 4),
                kind: MaskKind::Luminance,
                origin: MaskLayerOrigin::SvgClip,
                masks_backdrop: true,
            }),
            effects,
            sorting_root,
            clip_path,
        );
        let background_color_animation =
            tree.append_effect(EffectNodeData::BackgroundColorAnimation, mask, sorting_root, clip_path);
        tree.root_isolation_effect = Some(root_effect);
        tree.set_visual_animations(vec![
            VisualAnimation {
                target_kind: FfiVisualAnimationTargetKind::Opacity,
                node_indices: vec![effects.0],
                monotonic_time_at_anchor_ns: 1_000_000,
                local_time_at_anchor_ms: 2.5,
                playback_rate: 1.5,
                timing: AnimationEffectTiming {
                    start_delay: TimeValue::milliseconds(3.0),
                    iteration_duration: TimeValue::milliseconds(400.0),
                    iteration_count: f64::INFINITY,
                    iteration_start: 0.25,
                    playback_direction: PlaybackDirection::Alternate,
                    fill_mode: FillMode::Backwards,
                    ..VisualAnimation::default().timing
                },
                easing: Easing::CubicBezier {
                    x1: 0.1,
                    y1: 0.2,
                    x2: 0.3,
                    y2: 0.4,
                },
                keyframes: vec![
                    animation_keyframe(0.0, Easing::default(), VisualAnimationValue::Opacity(0.0)),
                    animation_keyframe(
                        0.5,
                        Easing::Steps {
                            interval_count: 3,
                            position: 2,
                        },
                        VisualAnimationValue::Opacity(0.5),
                    ),
                    animation_keyframe(1.0, Easing::default(), VisualAnimationValue::Opacity(1.0)),
                ],
            },
            VisualAnimation {
                target_kind: FfiVisualAnimationTargetKind::BackgroundColor,
                node_indices: vec![background_color_animation.0],
                timing: AnimationEffectTiming {
                    iteration_duration: TimeValue::milliseconds(100.0),
                    iteration_count: 2.0,
                    ..VisualAnimation::default().timing
                },
                keyframes: vec![
                    animation_keyframe(
                        0.0,
                        Easing::default(),
                        VisualAnimationValue::BackgroundColor(Color::from_rgba(1, 2, 3, 4)),
                    ),
                    animation_keyframe(
                        1.0,
                        Easing::default(),
                        VisualAnimationValue::BackgroundColor(Color::from_rgb(5, 6, 7)),
                    ),
                ],
                ..VisualAnimation::default()
            },
            VisualAnimation {
                target_kind: FfiVisualAnimationTargetKind::Filter,
                node_indices: vec![root_effect.0],
                timing: AnimationEffectTiming {
                    iteration_duration: TimeValue::milliseconds(100.0),
                    ..VisualAnimation::default().timing
                },
                keyframes: vec![
                    animation_keyframe(0.0, Easing::default(), VisualAnimationValue::Filter(Vec::new())),
                    animation_keyframe(
                        1.0,
                        Easing::default(),
                        VisualAnimationValue::Filter(vec![
                            FfiFilterFunction {
                                kind: FfiFilterFunctionKind::DropShadow,
                                amount: 1.0,
                                offset_x: 2.0,
                                offset_y: 3.0,
                                color: Color::from_rgb(8, 9, 10),
                                color_operation: ColorFilterType::Brightness,
                            },
                            FfiFilterFunction {
                                kind: FfiFilterFunctionKind::Color,
                                amount: 0.5,
                                offset_x: 0.0,
                                offset_y: 0.0,
                                color: Color::TRANSPARENT,
                                color_operation: ColorFilterType::Sepia,
                            },
                        ]),
                    ),
                ],
                ..VisualAnimation::default()
            },
            VisualAnimation {
                target_kind: FfiVisualAnimationTargetKind::Transform,
                node_indices: vec![VISUAL_VIEWPORT_NODE_INDEX.0],
                timing: AnimationEffectTiming {
                    iteration_duration: TimeValue::milliseconds(100.0),
                    ..VisualAnimation::default().timing
                },
                keyframes: vec![
                    animation_keyframe(0.0, Easing::default(), translate_and_rotate(0.0, 0.0, 0.0)),
                    animation_keyframe(1.0, Easing::default(), translate_and_rotate(1.0, 2.0, 3.0)),
                ],
                ..VisualAnimation::default()
            },
        ]);
        tree
    }

    fn animation_keyframe(offset: f64, easing: Easing, value: VisualAnimationValue) -> VisualAnimationKeyframe {
        VisualAnimationKeyframe { offset, easing, value }
    }

    fn translate_and_rotate(x: f32, y: f32, angle: f32) -> VisualAnimationValue {
        VisualAnimationValue::Transform(vec![
            VisualAnimationTransformOperation::new(FfiVisualAnimationTransformOperationKind::Translate, &[x, y])
                .unwrap(),
            VisualAnimationTransformOperation::new(FfiVisualAnimationTransformOperationKind::Rotate, &[angle]).unwrap(),
        ])
    }

    fn with_animations(mut tree: VisualContextTree, animations: Vec<VisualAnimation>) -> Vec<u8> {
        tree.set_visual_animations(animations);
        tree.to_bytes()
    }

    fn spatial_data_matches(a: &SpatialData, b: &SpatialData) -> bool {
        match (a, b) {
            (SpatialData::Scroll(_), SpatialData::Scroll(_)) => true,
            (SpatialData::Sticky(a), SpatialData::Sticky(b)) => {
                StickyData {
                    state_slot: NO_SCROLL_STATE_SLOT,
                    owner_paintable: b.owner_paintable,
                    ..*a
                } == *b
            }
            (SpatialData::Transform(a), SpatialData::Transform(b)) => a == b,
            (SpatialData::Perspective(a), SpatialData::Perspective(b)) => a == b,
            (SpatialData::BackfaceVisibility(a), SpatialData::BackfaceVisibility(b)) => a == b,
            (SpatialData::AnchorScrollShift(a), SpatialData::AnchorScrollShift(b)) => a == b,
            (SpatialData::Dead, SpatialData::Dead) => true,
            _ => false,
        }
    }

    fn clip_data_matches(a: &ClipNodeData, b: &ClipNodeData) -> bool {
        match (a, b) {
            (ClipNodeData::Rect(a), ClipNodeData::Rect(b)) => a == b,
            (ClipNodeData::Path(a), ClipNodeData::Path(b)) => {
                a.bounding_rect == b.bounding_rect && a.fill_rule == b.fill_rule
            }
            (ClipNodeData::Dead, ClipNodeData::Dead) => true,
            _ => false,
        }
    }

    fn effect_data_matches(a: &EffectNodeData, b: &EffectNodeData) -> bool {
        match (a, b) {
            (EffectNodeData::Effects(a), EffectNodeData::Effects(b)) => {
                a.isolation == b.isolation
                    && a.opacity == b.opacity
                    && a.blend_mode == b.blend_mode
                    && a.filter == b.filter
                    && a.backdrop_filter == b.backdrop_filter
            }
            (EffectNodeData::Mask(a), EffectNodeData::Mask(b)) => a == b,
            (EffectNodeData::BackgroundColorAnimation, EffectNodeData::BackgroundColorAnimation) => true,
            (EffectNodeData::Dead, EffectNodeData::Dead) => true,
            _ => false,
        }
    }

    fn assert_trees_match(a: &VisualContextTree, b: &VisualContextTree) {
        assert_eq!(a.structural_epoch, b.structural_epoch);
        assert_eq!(a.root_isolation_effect, b.root_isolation_effect);
        assert_eq!(a.spatial_nodes.len(), b.spatial_nodes.len());
        assert_eq!(a.clip_nodes.len(), b.clip_nodes.len());
        assert_eq!(a.effect_nodes.len(), b.effect_nodes.len());
        assert_eq!(a.live_spatial_node_count(), b.live_spatial_node_count());
        assert_eq!(a.live_clip_node_count(), b.live_clip_node_count());
        assert_eq!(a.live_effect_node_count(), b.live_effect_node_count());
        for (node, other) in a.spatial_nodes.iter().zip(&b.spatial_nodes) {
            assert_eq!(node.parent, other.parent);
            assert!(spatial_data_matches(&node.data, &other.data));
        }
        for (node, other) in a.clip_nodes.iter().zip(&b.clip_nodes) {
            assert_eq!(node.parent, other.parent);
            assert_eq!(node.spatial, other.spatial);
            assert_eq!(node.clips_everything, other.clips_everything);
            assert!(clip_data_matches(&node.data, &other.data));
        }
        for (node, other) in a.effect_nodes.iter().zip(&b.effect_nodes) {
            assert_eq!(node.parent, other.parent);
            assert_eq!(node.spatial, other.spatial);
            assert_eq!(node.local_clip, other.local_clip);
            assert!(effect_data_matches(&node.data, &other.data));
        }
        assert_eq!(a.visual_animations(), b.visual_animations());
        assert_eq!(a.compositor_scroll_table(), b.compositor_scroll_table());
    }

    fn tree_with_compositor_scroll_table() -> VisualContextTree {
        let mut tree = VisualContextTree::create(transform(FloatMatrix4x4::identity()));
        let viewport = tree.append_spatial(
            SpatialData::Scroll(ScrollData {
                state_slot: NO_SCROLL_STATE_SLOT,
                owner_paintable: NodeSlotId::INVALID,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let nested = tree.append_spatial(
            SpatialData::Scroll(ScrollData {
                state_slot: NO_SCROLL_STATE_SLOT,
                owner_paintable: NodeSlotId::INVALID,
            }),
            viewport,
        );
        let viewport_record = CompositorScrollNodeRecord {
            scrollable_node_id: UniqueNodeId(17),
            scroll_node_index: viewport,
            parent_scroll_node_index: VISUAL_VIEWPORT_NODE_INDEX,
            scrollport_rect: IntRect::new(0, 0, 800, 600),
            min_scroll_offset: FloatPoint { x: -12.5, y: 0.0 },
            max_scroll_offset: FloatPoint { x: 42.25, y: 1200.0 },
            scroll_node_kind: CompositorScrollNodeKind::Viewport,
            pseudo_element_type: 0,
            is_viewport: true,
            can_be_wheel_scrolled_horizontally: false,
            can_be_wheel_scrolled_vertically: true,
            chains_scroll_to_ancestors_horizontally: false,
            chains_scroll_to_ancestors_vertically: true,
            shows_overscroll_effect_horizontally: true,
            shows_overscroll_effect_vertically: false,
        };
        tree.set_compositor_scroll_table(CompositorScrollTable {
            nodes: vec![
                viewport_record,
                CompositorScrollNodeRecord {
                    scrollable_node_id: UniqueNodeId(23),
                    scroll_node_index: nested,
                    parent_scroll_node_index: viewport,
                    scrollport_rect: IntRect::new(10, 20, 200, 100),
                    scroll_node_kind: CompositorScrollNodeKind::PseudoElement,
                    pseudo_element_type: 1,
                    is_viewport: false,
                    ..viewport_record
                },
            ]
            .into(),
            snap_containers: vec![CompositorSnapContainerRecord {
                scroll_node_index: nested,
                snapport: FfiCssPixelRect {
                    x: CssPixels::from_raw(1),
                    y: CssPixels::from_integer(2),
                    width: CssPixels::from_integer(200),
                    height: CssPixels::from_integer(100),
                },
                min_scroll_offset: FfiCssPixelPoint {
                    x: CssPixels::from_raw(-1),
                    y: CssPixels::from_raw(-2),
                },
                max_scroll_offset: FfiCssPixelPoint {
                    x: CssPixels::from_integer(10),
                    y: CssPixels::from_integer(300),
                },
                strictness: 2,
                snaps_x: false,
                snaps_y: true,
                horizontal_writing_mode: true,
                first_area_index: 0,
                area_count: 2,
            }]
            .into(),
            snap_areas: vec![
                CompositorSnapAreaRecord {
                    area_node_id: UniqueNodeId(24),
                    pseudo_element_type: 1,
                    rect: FfiCssPixelRect {
                        x: CssPixels::from_raw(-17),
                        y: CssPixels::from_integer(30),
                        width: CssPixels::from_integer(100),
                        height: CssPixels::from_integer(50),
                    },
                    align_x: 1,
                    align_y: 2,
                    always_stop: true,
                },
                CompositorSnapAreaRecord {
                    area_node_id: UniqueNodeId(25),
                    pseudo_element_type: 0,
                    rect: FfiCssPixelRect {
                        x: CssPixels::from_integer(40),
                        y: CssPixels::from_integer(200),
                        width: CssPixels::from_integer(80),
                        height: CssPixels::from_raw(2001),
                    },
                    align_x: 0,
                    align_y: 3,
                    always_stop: false,
                },
            ]
            .into(),
        });
        tree
    }

    #[test]
    fn compositor_scroll_table_round_trips() {
        let tree = tree_with_compositor_scroll_table();
        let bytes = tree.to_bytes();
        let decoded = VisualContextTree::from_bytes(&bytes).expect("a scroll table decodes");
        assert_trees_match(&tree, &decoded);
        assert_eq!(decoded.to_bytes(), bytes);
        let cloned = tree.clone();
        assert!(std::sync::Arc::ptr_eq(
            &tree.compositor_scroll_table.nodes,
            &cloned.compositor_scroll_table.nodes
        ));
        assert!(std::sync::Arc::ptr_eq(
            &tree.compositor_scroll_table.snap_containers,
            &cloned.compositor_scroll_table.snap_containers
        ));
        assert!(std::sync::Arc::ptr_eq(
            &tree.compositor_scroll_table.snap_areas,
            &cloned.compositor_scroll_table.snap_areas
        ));
    }

    #[test]
    fn compositor_scroll_table_setter_changes_values_without_changing_structure() {
        let mut tree = tree_with_compositor_scroll_table();
        let original = tree.clone();
        let epoch = tree.structural_epoch;
        assert!(!tree.set_compositor_scroll_table(tree.compositor_scroll_table().clone()));
        assert_eq!(tree.structural_epoch, epoch);
        let mut changed = tree.compositor_scroll_table().clone();
        std::sync::Arc::make_mut(&mut changed.nodes)[1].max_scroll_offset.y += 1.0;
        assert!(tree.set_compositor_scroll_table(changed));
        assert_eq!(tree.structural_epoch, epoch);
        assert_eq!(tree.node_count(), original.node_count());
        assert_eq!(tree.live_spatial_node_count(), original.live_spatial_node_count());
        assert_eq!(tree.spatial_slots.free, original.spatial_slots.free);
        for (node, other) in tree.spatial_nodes.iter().zip(&original.spatial_nodes) {
            assert_eq!(node.parent, other.parent);
            assert!(spatial_data_matches(&node.data, &other.data));
        }
        assert_ne!(tree.compositor_scroll_table(), original.compositor_scroll_table());
    }

    #[test]
    fn invalid_compositor_scroll_table_references_and_ranges_are_rejected() {
        let rejected = |change: fn(&mut VisualContextTree)| {
            let mut tree = tree_with_compositor_scroll_table();
            change(&mut tree);
            assert!(tree.node_references_are_consistent());
            assert!(VisualContextTree::from_bytes(&tree.to_bytes()).is_err());
        };
        rejected(|tree| {
            std::sync::Arc::make_mut(&mut tree.compositor_scroll_table.nodes)[1].scroll_node_index =
                SpatialNodeIndex(999)
        });
        rejected(|tree| {
            std::sync::Arc::make_mut(&mut tree.compositor_scroll_table.nodes)[0].scroll_node_index =
                VISUAL_VIEWPORT_NODE_INDEX
        });
        rejected(|tree| {
            tree.spatial_nodes[2].parent = VISUAL_VIEWPORT_NODE_INDEX;
            tree.spatial_nodes[1].data = SpatialData::Dead;
        });
        rejected(|tree| std::sync::Arc::make_mut(&mut tree.compositor_scroll_table.nodes).swap(0, 1));
        rejected(|tree| {
            std::sync::Arc::make_mut(&mut tree.compositor_scroll_table.nodes)[1].scroll_node_index = SpatialNodeIndex(1)
        });
        rejected(|tree| std::sync::Arc::make_mut(&mut tree.compositor_scroll_table.nodes)[1].is_viewport = true);
        rejected(|tree| {
            std::sync::Arc::make_mut(&mut tree.compositor_scroll_table.nodes)[1].parent_scroll_node_index =
                SpatialNodeIndex(999)
        });
        rejected(|tree| {
            std::sync::Arc::make_mut(&mut tree.compositor_scroll_table.nodes)[0].parent_scroll_node_index =
                SpatialNodeIndex(2)
        });
        rejected(|tree| {
            std::sync::Arc::make_mut(&mut tree.compositor_scroll_table.nodes)[1].parent_scroll_node_index =
                SpatialNodeIndex(2)
        });
        rejected(|tree| {
            let unlisted_scroll = tree.append_spatial(
                SpatialData::Scroll(ScrollData {
                    state_slot: NO_SCROLL_STATE_SLOT,
                    owner_paintable: NodeSlotId::INVALID,
                }),
                VISUAL_VIEWPORT_NODE_INDEX,
            );
            std::sync::Arc::make_mut(&mut tree.compositor_scroll_table.snap_containers)[0].scroll_node_index =
                unlisted_scroll;
        });
        rejected(|tree| std::sync::Arc::make_mut(&mut tree.compositor_scroll_table.snap_containers)[0].area_count = 3);
        rejected(|tree| std::sync::Arc::make_mut(&mut tree.compositor_scroll_table.snap_containers)[0].area_count = 1);
        rejected(|tree| {
            std::sync::Arc::make_mut(&mut tree.compositor_scroll_table.snap_containers)[0].first_area_index = 1
        });
        rejected(|tree| {
            std::sync::Arc::make_mut(&mut tree.compositor_scroll_table.snap_containers)[0].first_area_index = u32::MAX
        });
        rejected(|tree| {
            let container = tree.compositor_scroll_table.snap_containers[0];
            tree.compositor_scroll_table.snap_containers = vec![
                CompositorSnapContainerRecord {
                    scroll_node_index: SpatialNodeIndex(1),
                    ..container
                },
                CompositorSnapContainerRecord {
                    first_area_index: 1,
                    area_count: 1,
                    ..container
                },
            ]
            .into();
        });
        rejected(|tree| {
            let container = tree.compositor_scroll_table.snap_containers[0];
            tree.compositor_scroll_table.snap_containers = vec![container, container].into();
        });
        rejected(|tree| {
            let container = tree.compositor_scroll_table.snap_containers[0];
            tree.compositor_scroll_table.snap_containers = vec![
                container,
                CompositorSnapContainerRecord {
                    scroll_node_index: SpatialNodeIndex(1),
                    first_area_index: 2,
                    area_count: 0,
                    ..container
                },
            ]
            .into();
        });
        rejected(|tree| tree.compositor_scroll_table.snap_containers = Default::default());
        rejected(|tree| std::sync::Arc::make_mut(&mut tree.compositor_scroll_table.snap_containers)[0].strictness = 3);
        rejected(|tree| std::sync::Arc::make_mut(&mut tree.compositor_scroll_table.snap_areas)[0].align_x = 4);
        rejected(|tree| std::sync::Arc::make_mut(&mut tree.compositor_scroll_table.snap_areas)[0].align_y = 4);
        for value in [f32::NAN, f32::INFINITY, f32::NEG_INFINITY] {
            for coordinate in 0..4 {
                let mut tree = tree_with_compositor_scroll_table();
                let node = &mut std::sync::Arc::make_mut(&mut tree.compositor_scroll_table.nodes)[0];
                match coordinate {
                    0 => node.min_scroll_offset.x = value,
                    1 => node.min_scroll_offset.y = value,
                    2 => node.max_scroll_offset.x = value,
                    _ => node.max_scroll_offset.y = value,
                }
                assert!(VisualContextTree::from_bytes(&tree.to_bytes()).is_err());
            }
        }
    }

    #[test]
    fn malformed_compositor_scroll_table_bytes_are_rejected() {
        let tree = tree_with_compositor_scroll_table();
        let bytes = tree.to_bytes();
        let mut without_table = tree.clone();
        without_table.set_compositor_scroll_table(CompositorScrollTable::default());
        let empty_bytes = without_table.to_bytes();
        assert_eq!(&empty_bytes[empty_bytes.len() - 12..], &[0; 12]);
        let section_start = empty_bytes.len() - 12;
        for end in section_start..bytes.len() {
            assert!(
                VisualContextTree::from_bytes(&bytes[..end]).is_err(),
                "truncation at {end} decoded"
            );
        }
        let nodes_start = section_start + 4;
        let container_count_offset = nodes_start + 2 * CompositorScrollNodeRecord::MIN_ENCODED_SIZE;
        let container_start = container_count_offset + 4;
        let area_count_offset = container_start + CompositorSnapContainerRecord::MIN_ENCODED_SIZE;
        let area_start = area_count_offset + 4;
        for offset in [section_start, container_count_offset, area_count_offset] {
            let mut invalid = bytes.clone();
            invalid[offset..offset + 4].copy_from_slice(&u32::MAX.to_ne_bytes());
            assert!(VisualContextTree::from_bytes(&invalid).is_err());
        }
        for offset in std::iter::once(nodes_start + 48)
            .chain((nodes_start + 50)..(nodes_start + 57))
            .chain((container_start + 36)..(container_start + 40))
            .chain((area_start + 25)..(area_start + 28))
        {
            let mut invalid = bytes.clone();
            invalid[offset] = u8::MAX;
            assert!(
                VisualContextTree::from_bytes(&invalid).is_err(),
                "invalid byte at {offset} decoded"
            );
        }
    }

    #[test]
    fn a_tree_without_animations_round_trips() {
        let mut tree = tree_with_every_node_kind();
        tree.clear_visual_animations();
        let bytes = tree.to_bytes();
        let decoded = VisualContextTree::from_bytes(&bytes).expect("a serialized tree decodes");
        assert!(!decoded.has_visual_animations());
        assert_eq!(decoded.to_bytes(), bytes);
    }

    #[test]
    fn an_animation_that_cannot_be_sampled_is_rejected() {
        let tree = tree_with_every_node_kind();
        let valid = tree.visual_animations()[0].clone();
        assert!(VisualContextTree::from_bytes(&with_animations(tree.clone(), vec![valid.clone()])).is_ok());

        let rejected = |animation: VisualAnimation| {
            assert!(
                VisualContextTree::from_bytes(&with_animations(tree.clone(), vec![animation.clone()])).is_err(),
                "{animation:?} decoded"
            );
        };
        rejected(VisualAnimation {
            node_indices: Vec::new(),
            ..valid.clone()
        });
        rejected(VisualAnimation {
            timing: AnimationEffectTiming {
                iteration_duration: TimeValue::milliseconds(0.0),
                ..valid.timing
            },
            ..valid.clone()
        });
        rejected(VisualAnimation {
            playback_rate: f64::NAN,
            ..valid.clone()
        });
        rejected(VisualAnimation {
            keyframes: valid.keyframes[1..].to_vec(),
            ..valid.clone()
        });
        rejected(VisualAnimation {
            easing: Easing::Linear(vec![FfiLinearEasingPoint {
                input: 0.0,
                output: 0.0,
            }]),
            ..valid.clone()
        });
    }

    #[test]
    fn an_animation_naming_the_wrong_node_kind_is_rejected() {
        let tree = tree_with_every_node_kind();
        let opacity = tree.visual_animations()[0].clone();
        let background_color = tree.visual_animations()[1].clone();
        let transform = tree.visual_animations()[3].clone();

        let rejected = |animation: VisualAnimation| {
            assert!(
                VisualContextTree::from_bytes(&with_animations(tree.clone(), vec![animation.clone()])).is_err(),
                "{animation:?} decoded"
            );
        };
        // An opacity animation of the background color effect, and the reverse.
        rejected(VisualAnimation {
            node_indices: background_color.node_indices.clone(),
            ..opacity.clone()
        });
        rejected(VisualAnimation {
            node_indices: opacity.node_indices.clone(),
            ..background_color
        });
        // A transform animation of a synthetic plane, and of a node past the tree.
        rejected(VisualAnimation {
            node_indices: vec![SpatialNodeIndex(2).0],
            ..transform.clone()
        });
        rejected(VisualAnimation {
            node_indices: vec![tree.spatial_nodes.len() as u32],
            ..transform
        });
        rejected(VisualAnimation {
            node_indices: vec![tree.effect_nodes.len() as u32],
            ..opacity
        });
    }

    #[test]
    fn an_unknown_animation_enum_value_is_rejected() {
        let tree = tree_with_every_node_kind();
        let mut bytes = with_animations(tree.clone(), vec![tree.visual_animations()[0].clone()]);
        let mut tree_without_animations = tree.clone();
        tree_without_animations.clear_visual_animations();
        // The target kind is the first byte after the animation count.
        let animation_start = tree_without_animations.to_bytes().len() - 12;
        assert_eq!(bytes[animation_start], FfiVisualAnimationTargetKind::Opacity as u8);
        bytes[animation_start] = 4;
        assert_eq!(
            VisualContextTree::from_bytes(&bytes).err(),
            Some(DecodeError::InvalidValue("FfiVisualAnimationTargetKind"))
        );
    }

    #[test]
    fn a_tree_with_every_node_kind_round_trips() {
        let tree = tree_with_every_node_kind();
        let bytes = tree.to_bytes();
        let decoded = VisualContextTree::from_bytes(&bytes).expect("a serialized tree decodes");
        assert_trees_match(&tree, &decoded);
        assert_eq!(decoded.to_bytes(), bytes);
    }

    #[test]
    fn a_filter_that_the_player_cannot_build_is_rejected() {
        let blur = Filter::blur(2.0, 2.0, None).serialize();
        let truncated = blur[..blur.len() - 1].to_vec();
        let tree_with = |filter: &[u8], backdrop_filter: &[u8]| {
            let mut tree = tree_with_every_node_kind();
            for node in &mut tree.effect_nodes {
                if let EffectNodeData::Effects(effects) = &mut node.data
                    && let Some(backdrop) = &mut effects.backdrop_filter
                {
                    effects.filter = Some(std::sync::Arc::new(filter.to_vec()));
                    backdrop.filter = std::sync::Arc::new(backdrop_filter.to_vec());
                }
            }
            tree.to_bytes()
        };
        assert!(VisualContextTree::from_bytes(&tree_with(&blur, &blur)).is_ok());
        assert!(VisualContextTree::from_bytes(&tree_with(&truncated, &blur)).is_err());
        assert!(VisualContextTree::from_bytes(&tree_with(&blur, &truncated)).is_err());
        assert!(VisualContextTree::from_bytes(&tree_with(&blur, &[])).is_err());
    }

    #[test]
    fn a_tree_with_tombstones_round_trips_and_keeps_its_live_counts() {
        let mut tree = tree_with_every_node_kind();
        let scroll_node = SpatialNodeIndex(1);
        let stale_transform = tree.append_spatial(
            SpatialData::Transform(transform(FloatMatrix4x4::identity())),
            scroll_node,
        );
        let stale_effect = tree.append_effect(
            EffectNodeData::Effects(EffectsData {
                isolation: false,
                opacity: 0.5,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
                backdrop_filter: None,
            }),
            EffectNodeIndex::NONE,
            scroll_node,
            ClipNodeIndex::NONE,
        );
        let stale_clip = tree.append_clip(
            ClipNodeData::rect_clip(FloatRect::new(0.0, 0.0, 1.0, 1.0)),
            ClipNodeIndex::NONE,
            scroll_node,
        );
        assert!(tree.tombstone_spatial_slot(stale_transform));
        assert!(tree.tombstone_effect_slot(stale_effect));
        assert!(tree.tombstone_clip_slot(stale_clip));
        let bytes = tree.to_bytes();
        let decoded = VisualContextTree::from_bytes(&bytes).expect("a serialized tree decodes");
        assert_trees_match(&tree, &decoded);
        assert!(!decoded.spatial_is_live(stale_transform));
        assert!(!decoded.clip_is_live(stale_clip));
        assert!(!decoded.effect_is_live(stale_effect));
        assert_eq!(decoded.dead_node_count(), 3);
        assert_eq!(decoded.to_bytes(), bytes);
    }

    #[test]
    fn a_live_node_referencing_a_tombstone_is_rejected() {
        let tree_with_tombstoned_target = |make_referencing_data: &dyn Fn(SpatialNodeIndex) -> SpatialData| {
            let mut tree = VisualContextTree::create(transform(FloatMatrix4x4::identity()));
            let target = tree.append_spatial(
                SpatialData::Scroll(ScrollData {
                    state_slot: NO_SCROLL_STATE_SLOT,
                    owner_paintable: NodeSlotId::INVALID,
                }),
                VISUAL_VIEWPORT_NODE_INDEX,
            );
            tree.append_spatial(make_referencing_data(target), VISUAL_VIEWPORT_NODE_INDEX);
            assert!(tree.tombstone_spatial_slot(target));
            tree
        };
        let plane_root = tree_with_tombstoned_target(&|target| {
            SpatialData::BackfaceVisibility(BackfaceVisibilityData {
                plane_root_index: target,
                flattens_inherited_transform: false,
            })
        });
        assert!(VisualContextTree::from_bytes(&encode_tree(&plane_root)).is_err());
        let sorting_root = tree_with_tombstoned_target(&|target| {
            SpatialData::Transform(TransformData {
                sorting_context_root_index: Some(target),
                ..transform(FloatMatrix4x4::identity())
            })
        });
        assert!(VisualContextTree::from_bytes(&encode_tree(&sorting_root)).is_err());
        let anchor = tree_with_tombstoned_target(&|target| {
            SpatialData::AnchorScrollShift(AnchorScrollShift {
                scroll_node_index: target,
                negate: false,
                compensate_horizontal_scroll: true,
                compensate_vertical_scroll: true,
            })
        });
        assert!(VisualContextTree::from_bytes(&encode_tree(&anchor)).is_err());
        let scroller = tree_with_tombstoned_target(&|target| {
            SpatialData::Sticky(StickyData::unconstrained(
                target,
                None,
                NO_SCROLL_STATE_SLOT,
                NodeSlotId::INVALID,
            ))
        });
        assert!(VisualContextTree::from_bytes(&encode_tree(&scroller)).is_err());
    }

    #[test]
    fn a_live_node_under_a_tombstone_is_rejected() {
        let mut child_under_tombstone = VisualContextTree::create(transform(FloatMatrix4x4::identity()));
        let parent = child_under_tombstone.append_spatial(
            SpatialData::Transform(transform(FloatMatrix4x4::identity())),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        child_under_tombstone.append_spatial(SpatialData::Transform(transform(FloatMatrix4x4::identity())), parent);
        assert!(child_under_tombstone.tombstone_spatial_slot(parent));
        assert!(VisualContextTree::from_bytes(&encode_tree(&child_under_tombstone)).is_err());

        let effects = || {
            EffectNodeData::Effects(EffectsData {
                isolation: false,
                opacity: 1.0,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
                backdrop_filter: None,
            })
        };
        let rect = || ClipNodeData::rect_clip(FloatRect::new(0.0, 0.0, 1.0, 1.0));
        let fresh = || VisualContextTree::create(transform(FloatMatrix4x4::identity()));
        let rejected = |tree: &VisualContextTree| {
            assert!(!tree.node_references_are_consistent());
            assert!(VisualContextTree::from_bytes(&encode_tree(tree)).is_err());
        };

        let mut effect_under_tombstone = fresh();
        let parent_effect = effect_under_tombstone.append_effect(
            effects(),
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );
        effect_under_tombstone.append_effect(
            effects(),
            parent_effect,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );
        assert!(effect_under_tombstone.tombstone_effect_slot(parent_effect));
        rejected(&effect_under_tombstone);

        let mut clip_under_tombstone = fresh();
        let parent_clip = clip_under_tombstone.append_clip(rect(), ClipNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX);
        clip_under_tombstone.append_clip(rect(), parent_clip, VISUAL_VIEWPORT_NODE_INDEX);
        assert!(clip_under_tombstone.tombstone_clip_slot(parent_clip));
        rejected(&clip_under_tombstone);

        let mut nodes_in_tombstone = fresh();
        let spatial = nodes_in_tombstone.append_spatial(
            SpatialData::Transform(transform(FloatMatrix4x4::identity())),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let mut clip_in_tombstone = nodes_in_tombstone.clone();
        clip_in_tombstone.append_clip(rect(), ClipNodeIndex::NONE, spatial);
        assert!(clip_in_tombstone.tombstone_spatial_slot(spatial));
        rejected(&clip_in_tombstone);
        nodes_in_tombstone.append_effect(effects(), EffectNodeIndex::NONE, spatial, ClipNodeIndex::NONE);
        assert!(nodes_in_tombstone.tombstone_spatial_slot(spatial));
        rejected(&nodes_in_tombstone);

        let mut effect_under_tombstoned_local_clip = fresh();
        let output_clip =
            effect_under_tombstoned_local_clip.append_clip(rect(), ClipNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX);
        effect_under_tombstoned_local_clip.append_effect(
            effects(),
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            output_clip,
        );
        assert!(effect_under_tombstoned_local_clip.tombstone_clip_slot(output_clip));
        rejected(&effect_under_tombstoned_local_clip);

        let mut tombstoned_isolation_effect = fresh();
        let isolation_effect = tombstoned_isolation_effect.append_effect(
            effects(),
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );
        assert!(tombstoned_isolation_effect.tombstone_effect_slot(isolation_effect));
        tombstoned_isolation_effect.root_isolation_effect = Some(isolation_effect);
        rejected(&tombstoned_isolation_effect);
    }

    #[test]
    fn truncated_bytes_are_rejected_without_panicking() {
        let bytes = tree_with_every_node_kind().to_bytes();
        for length in 0..bytes.len() {
            assert!(
                VisualContextTree::from_bytes(&bytes[..length]).is_err(),
                "a prefix of {length} bytes decoded"
            );
        }
    }

    #[test]
    fn trailing_bytes_are_rejected() {
        let mut bytes = tree_with_every_node_kind().to_bytes();
        bytes.push(0);
        assert!(VisualContextTree::from_bytes(&bytes).is_err());
    }

    fn encode_tree(tree: &VisualContextTree) -> Vec<u8> {
        tree.to_bytes()
    }

    fn encoded_size<T: Encode>(value: &T) -> usize {
        let mut encoder = Encoder::new();
        encoder.encode(value).unwrap();
        encoder.finish().0.len()
    }

    fn hostile_tree() -> VisualContextTree {
        let mut tree = VisualContextTree::create(transform(FloatMatrix4x4::identity()));
        tree.append_spatial(
            SpatialData::Scroll(ScrollData {
                state_slot: NO_SCROLL_STATE_SLOT,
                owner_paintable: NodeSlotId::INVALID,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        tree.append_spatial(
            SpatialData::Sticky(StickyData::unconstrained(
                SpatialNodeIndex(1),
                None,
                NO_SCROLL_STATE_SLOT,
                NodeSlotId::INVALID,
            )),
            SpatialNodeIndex(1),
        );
        // Clips: c0 at the root, c1 under it in s2. Effects: e0 at the root under no clip, e1 under
        // e0 begins under local clip c0.
        tree.append_clip(
            ClipNodeData::rect_clip(FloatRect::new(0.0, 0.0, 10.0, 10.0)),
            ClipNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        tree.append_clip(
            ClipNodeData::Rect(ClipData {
                rect: FloatRect::default(),
                corner_radii: CornerRadii::default(),
                mode: ClipMode::Intersect,
            }),
            ClipNodeIndex(0),
            SpatialNodeIndex(2),
        );
        tree.append_effect(
            EffectNodeData::Effects(EffectsData {
                isolation: false,
                opacity: 1.0,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
                backdrop_filter: None,
            }),
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );
        tree.append_effect(
            EffectNodeData::Effects(EffectsData {
                isolation: false,
                opacity: 0.5,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
                backdrop_filter: None,
            }),
            EffectNodeIndex(0),
            SpatialNodeIndex(2),
            ClipNodeIndex(0),
        );
        tree
    }

    #[test]
    fn references_to_nodes_stored_after_the_node_are_accepted() {
        let mut child_stored_below_its_parent = hostile_tree();
        child_stored_below_its_parent.spatial_nodes[1].parent = SpatialNodeIndex(2);
        child_stored_below_its_parent.spatial_nodes[2] = SpatialNode {
            data: SpatialData::Transform(transform(translation_matrix(5.0, 5.0, 0.0))),
            parent: VISUAL_VIEWPORT_NODE_INDEX,
        };
        child_stored_below_its_parent.clip_nodes[0].parent = ClipNodeIndex(1);
        child_stored_below_its_parent.clip_nodes[1].parent = ClipNodeIndex::NONE;
        child_stored_below_its_parent.effect_nodes[0].parent = EffectNodeIndex(1);
        child_stored_below_its_parent.effect_nodes[1].parent = EffectNodeIndex::NONE;
        child_stored_below_its_parent.effect_nodes[1].local_clip = ClipNodeIndex::NONE;
        child_stored_below_its_parent.effect_nodes[0].local_clip = ClipNodeIndex(1);
        let decoded = VisualContextTree::from_bytes(&encode_tree(&child_stored_below_its_parent))
            .expect("acyclic forward references decode");
        assert_eq!(decoded.spatial_dependency_order(), vec![0, 2, 1]);
        assert_eq!(decoded.clip_dependency_order(), vec![1, 0]);
        assert_eq!(decoded.effect_dependency_order(), vec![1, 0]);
        assert_eq!(
            super::super::WorldTransforms::new(&decoded, &[], None).transform_rect_to_viewport(
                SpatialNodeIndex(1),
                FloatRect::new(0.0, 0.0, 10.0, 10.0),
                super::super::IncludeVisualViewportTransform::Yes
            ),
            FloatRect::new(5.0, 5.0, 10.0, 10.0)
        );
    }

    #[test]
    fn reference_cycles_self_references_and_wrong_kinds_are_rejected() {
        let mut parent_cycle = hostile_tree();
        parent_cycle.spatial_nodes[1].parent = SpatialNodeIndex(2);
        assert!(VisualContextTree::from_bytes(&encode_tree(&parent_cycle)).is_err());

        let mut self_parented = hostile_tree();
        self_parented.spatial_nodes[1].parent = SpatialNodeIndex(1);
        assert!(VisualContextTree::from_bytes(&encode_tree(&self_parented)).is_err());

        let mut root_with_a_parent = hostile_tree();
        root_with_a_parent.spatial_nodes[0].parent = SpatialNodeIndex(1);
        assert!(VisualContextTree::from_bytes(&encode_tree(&root_with_a_parent)).is_err());

        let mut sticky_parent_cycle = hostile_tree();
        sticky_parent_cycle.spatial_nodes[1] = SpatialNode {
            data: SpatialData::Sticky(StickyData::unconstrained(
                VISUAL_VIEWPORT_NODE_INDEX,
                Some(SpatialNodeIndex(2)),
                NO_SCROLL_STATE_SLOT,
                NodeSlotId::INVALID,
            )),
            parent: VISUAL_VIEWPORT_NODE_INDEX,
        };
        sticky_parent_cycle.spatial_nodes[2] = SpatialNode {
            data: SpatialData::Sticky(StickyData::unconstrained(
                VISUAL_VIEWPORT_NODE_INDEX,
                Some(SpatialNodeIndex(1)),
                NO_SCROLL_STATE_SLOT,
                NodeSlotId::INVALID,
            )),
            parent: VISUAL_VIEWPORT_NODE_INDEX,
        };
        assert!(VisualContextTree::from_bytes(&encode_tree(&sticky_parent_cycle)).is_err());

        let mut parent_out_of_range = hostile_tree();
        parent_out_of_range.spatial_nodes[1].parent = SpatialNodeIndex(7);
        assert!(VisualContextTree::from_bytes(&encode_tree(&parent_out_of_range)).is_err());

        let mut plane_root_out_of_range = hostile_tree();
        plane_root_out_of_range.spatial_nodes[1].data = SpatialData::BackfaceVisibility(BackfaceVisibilityData {
            plane_root_index: SpatialNodeIndex(9),
            flattens_inherited_transform: false,
        });
        assert!(VisualContextTree::from_bytes(&encode_tree(&plane_root_out_of_range)).is_err());

        let mut root_is_not_a_transform = hostile_tree();
        root_is_not_a_transform.spatial_nodes[0].data = SpatialData::Scroll(ScrollData {
            state_slot: NO_SCROLL_STATE_SLOT,
            owner_paintable: NodeSlotId::INVALID,
        });
        assert!(VisualContextTree::from_bytes(&encode_tree(&root_is_not_a_transform)).is_err());

        let mut sorting_root_cycle = hostile_tree();
        sorting_root_cycle.spatial_nodes[0].data = SpatialData::Transform(TransformData {
            sorting_context_root_index: Some(SpatialNodeIndex(1)),
            ..transform(FloatMatrix4x4::identity())
        });
        assert!(VisualContextTree::from_bytes(&encode_tree(&sorting_root_cycle)).is_err());

        let mut sticky_scroller_is_not_a_scroll_node = hostile_tree();
        sticky_scroller_is_not_a_scroll_node.spatial_nodes[1].data = SpatialData::Perspective(PerspectiveData {
            matrix: FloatMatrix4x4::identity(),
            flattens_inherited_transform: false,
        });
        assert!(VisualContextTree::from_bytes(&encode_tree(&sticky_scroller_is_not_a_scroll_node)).is_err());

        let mut sticky_parent_is_not_sticky = hostile_tree();
        sticky_parent_is_not_sticky.spatial_nodes[2].data = SpatialData::Sticky(StickyData::unconstrained(
            SpatialNodeIndex(1),
            Some(SpatialNodeIndex(1)),
            NO_SCROLL_STATE_SLOT,
            NodeSlotId::INVALID,
        ));
        assert!(VisualContextTree::from_bytes(&encode_tree(&sticky_parent_is_not_sticky)).is_err());

        let mut plane_root_cycle = hostile_tree();
        plane_root_cycle.spatial_nodes[1].data = SpatialData::BackfaceVisibility(BackfaceVisibilityData {
            plane_root_index: SpatialNodeIndex(2),
            flattens_inherited_transform: false,
        });
        assert!(VisualContextTree::from_bytes(&encode_tree(&plane_root_cycle)).is_err());

        let mut plane_root_on_another_branch = hostile_tree();
        plane_root_on_another_branch.spatial_nodes[2].parent = VISUAL_VIEWPORT_NODE_INDEX;
        plane_root_on_another_branch.spatial_nodes[2].data = SpatialData::BackfaceVisibility(BackfaceVisibilityData {
            plane_root_index: SpatialNodeIndex(1),
            flattens_inherited_transform: false,
        });
        assert!(VisualContextTree::from_bytes(&encode_tree(&plane_root_on_another_branch)).is_err());

        let mut plane_root_on_the_root_path = hostile_tree();
        plane_root_on_the_root_path.spatial_nodes[2].data = SpatialData::BackfaceVisibility(BackfaceVisibilityData {
            plane_root_index: SpatialNodeIndex(1),
            flattens_inherited_transform: false,
        });
        assert!(VisualContextTree::from_bytes(&encode_tree(&plane_root_on_the_root_path)).is_ok());

        let mut anchor_node_names_itself = hostile_tree();
        anchor_node_names_itself.spatial_nodes[1].data = SpatialData::AnchorScrollShift(AnchorScrollShift {
            scroll_node_index: SpatialNodeIndex(1),
            negate: false,
            compensate_horizontal_scroll: true,
            compensate_vertical_scroll: true,
        });
        assert!(VisualContextTree::from_bytes(&encode_tree(&anchor_node_names_itself)).is_err());
    }

    #[test]
    fn clip_and_effect_reference_cycles_and_ranges_are_rejected() {
        let rejected = |tree: &VisualContextTree| assert!(VisualContextTree::from_bytes(&encode_tree(tree)).is_err());

        let mut clip_parent_cycle = hostile_tree();
        clip_parent_cycle.clip_nodes[0].parent = ClipNodeIndex(1);
        rejected(&clip_parent_cycle);

        let mut clip_parent_out_of_range = hostile_tree();
        clip_parent_out_of_range.clip_nodes[1].parent = ClipNodeIndex(4);
        rejected(&clip_parent_out_of_range);

        let mut clip_spatial_out_of_range = hostile_tree();
        clip_spatial_out_of_range.clip_nodes[1].spatial = SpatialNodeIndex(3);
        rejected(&clip_spatial_out_of_range);

        let mut effect_parent_cycle = hostile_tree();
        effect_parent_cycle.effect_nodes[0].parent = EffectNodeIndex(1);
        rejected(&effect_parent_cycle);

        let mut effect_parent_out_of_range = hostile_tree();
        effect_parent_out_of_range.effect_nodes[1].parent = EffectNodeIndex(4);
        rejected(&effect_parent_out_of_range);

        let mut effect_spatial_out_of_range = hostile_tree();
        effect_spatial_out_of_range.effect_nodes[1].spatial = SpatialNodeIndex(3);
        rejected(&effect_spatial_out_of_range);

        let mut local_clip_out_of_range = hostile_tree();
        local_clip_out_of_range.effect_nodes[1].local_clip = ClipNodeIndex(9);
        rejected(&local_clip_out_of_range);

        let mut isolation_effect_out_of_range = hostile_tree();
        isolation_effect_out_of_range.root_isolation_effect = Some(EffectNodeIndex(2));
        rejected(&isolation_effect_out_of_range);

        let mut isolation_effect_is_a_mask = hostile_tree();
        isolation_effect_is_a_mask.effect_nodes[0].data = EffectNodeData::Mask(MaskData {
            rect: IntRect::new(0, 0, 1, 1),
            kind: MaskKind::Alpha,
            origin: MaskLayerOrigin::CssMaskLayers,
            masks_backdrop: false,
        });
        isolation_effect_is_a_mask.root_isolation_effect = Some(EffectNodeIndex(0));
        rejected(&isolation_effect_is_a_mask);

        // The isolation layer must be a root effect.
        let mut isolation_effect_has_a_parent = hostile_tree();
        isolation_effect_has_a_parent.root_isolation_effect = Some(EffectNodeIndex(1));
        rejected(&isolation_effect_has_a_parent);

        let mut isolation_effect_at_the_root = hostile_tree();
        isolation_effect_at_the_root.root_isolation_effect = Some(EffectNodeIndex(0));
        assert!(VisualContextTree::from_bytes(&encode_tree(&isolation_effect_at_the_root)).is_ok());
    }

    #[test]
    fn out_of_range_discriminants_and_absurd_counts_are_rejected() {
        let tree = hostile_tree();
        let bytes = tree.to_bytes();
        let count_size = size_of::<u32>();
        let index_size = size_of::<u32>();
        let spatial_count_offset = encoded_size(&tree.structural_epoch) + encoded_size(&tree.root_isolation_effect);
        let clip_count_offset = spatial_count_offset + encoded_size(&tree.spatial_nodes);
        let effect_count_offset = clip_count_offset + encoded_size(&tree.clip_nodes);

        let root_spatial_kind_offset = spatial_count_offset + count_size + index_size;
        let mut bad_spatial_kind = bytes.clone();
        bad_spatial_kind[root_spatial_kind_offset] = 200;
        assert_eq!(
            VisualContextTree::from_bytes(&bad_spatial_kind).err(),
            Some(DecodeError::InvalidValue("SpatialData"))
        );

        for count_offset in [spatial_count_offset, clip_count_offset, effect_count_offset] {
            let mut absurd_count = bytes.clone();
            absurd_count[count_offset..count_offset + count_size].copy_from_slice(&u32::MAX.to_ne_bytes());
            assert_eq!(
                VisualContextTree::from_bytes(&absurd_count).err(),
                Some(DecodeError::UnexpectedEnd),
                "an absurd count at byte {count_offset} decoded"
            );
        }

        // A clip node's kind follows its parent and spatial indices.
        let first_clip_kind_offset = clip_count_offset + count_size + 2 * index_size;
        let mut bad_clip_kind = bytes.clone();
        bad_clip_kind[first_clip_kind_offset] = 200;
        assert_eq!(
            VisualContextTree::from_bytes(&bad_clip_kind).err(),
            Some(DecodeError::InvalidValue("ClipNodeData"))
        );
    }
}
