/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::{
    ClipMode, ClipNodeData, ClipNodeIndex, EffectNodeData, EffectNodeIndex, MaskLayerOrigin, SpatialData,
    SpatialNodeIndex, TransformDataRole, VISUAL_VIEWPORT_NODE_INDEX, VisualContextTree,
};
use crate::painting::display_list::commands::DisplayListCommandRun;
use crate::painting::dump::{
    format_float_like_ak, push_float_point, push_float_rect, push_float_size, push_int_rect_components,
};
use libgfx_rust::{CompositingAndBlendingOperator, FloatPoint, FloatRect, FloatSize, IntRect, MaskKind};
use std::collections::{HashMap, HashSet};
use std::fmt::Write;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SlotKind {
    Spatial,
    Clip,
    Effect,
}

fn format_point(point: FloatPoint) -> String {
    let mut text = String::new();
    push_float_point(&mut text, point);
    text
}

fn format_size(size: FloatSize) -> String {
    let mut text = String::new();
    push_float_size(&mut text, size);
    text
}

fn format_rect(rect: FloatRect) -> String {
    let mut text = String::new();
    push_float_rect(&mut text, rect);
    text
}

fn format_spatial_node_index(index: SpatialNodeIndex) -> String {
    format!("s{}", index.0)
}

fn format_int_rect_components(rect: IntRect) -> String {
    let mut text = String::new();
    push_int_rect_components(&mut text, rect);
    text
}

impl VisualContextTree {
    pub fn dump_spatial_node(&self, index: SpatialNodeIndex) -> String {
        let mut text = String::new();
        match &self.spatial_nodes[index.0 as usize].data {
            SpatialData::Perspective(_) => text.push_str("perspective"),
            SpatialData::BackfaceVisibility(backface) => {
                let _ = write!(
                    text,
                    "backface-hidden plane_root={}",
                    format_spatial_node_index(backface.plane_root_index)
                );
            }
            SpatialData::Scroll(_) => text.push_str("scroll"),
            SpatialData::Sticky(sticky) => {
                let _ = write!(text, "sticky scroller={}", format_spatial_node_index(sticky.scroller));
                if let Some(parent_sticky) = sticky.parent_sticky {
                    let _ = write!(text, " parent_sticky={}", format_spatial_node_index(parent_sticky));
                }
                let _ = write!(
                    text,
                    " position_relative_to_scroller={} border_box_size={} scrollport_size={} containing_block_region={} needs_parent_offset_adjustment={} insets=[",
                    format_point(sticky.position_relative_to_scroller),
                    format_size(sticky.border_box_size),
                    format_size(sticky.scrollport_size),
                    format_rect(sticky.containing_block_region),
                    sticky.needs_parent_offset_adjustment
                );
                let mut is_first_inset = true;
                let mut append_inset = |side: &str, inset: Option<f32>| {
                    let Some(inset) = inset else {
                        return;
                    };
                    if !is_first_inset {
                        text.push_str(", ");
                    }
                    let _ = write!(text, "{side}={}", format_float_like_ak(inset));
                    is_first_inset = false;
                };
                append_inset("top", sticky.inset_top);
                append_inset("right", sticky.inset_right);
                append_inset("bottom", sticky.inset_bottom);
                append_inset("left", sticky.inset_left);
                text.push(']');
            }
            SpatialData::Transform(transform) => {
                let matrix = &transform.matrix.elements;
                let origin = transform.origin;
                let _ = write!(
                    text,
                    "{}=[{},{},{},{},{},{}] origin=({},{})",
                    if transform.role == TransformDataRole::SvgViewportTransform {
                        "svg-viewport-transform"
                    } else {
                        "transform"
                    },
                    format_float_like_ak(matrix[0][0]),
                    format_float_like_ak(matrix[0][1]),
                    format_float_like_ak(matrix[1][0]),
                    format_float_like_ak(matrix[1][1]),
                    format_float_like_ak(matrix[0][3]),
                    format_float_like_ak(matrix[1][3]),
                    format_float_like_ak(origin.x),
                    format_float_like_ak(origin.y)
                );
            }
            SpatialData::AnchorScrollShift(shift) => {
                let _ = write!(
                    text,
                    "anchor_scroll_shift(node_index={}{}{}{})",
                    format_spatial_node_index(shift.scroll_node_index),
                    if shift.negate { ", negate" } else { "" },
                    if shift.compensate_horizontal_scroll {
                        ""
                    } else {
                        ", no-x"
                    },
                    if shift.compensate_vertical_scroll { "" } else { ", no-y" }
                );
            }
            SpatialData::Dead => text.push_str("tombstone"),
        }
        text
    }

    pub fn dump_clip_node(&self, index: ClipNodeIndex) -> String {
        let mut text = String::new();
        match &self.clip_nodes[index.0 as usize].data {
            ClipNodeData::Rect(clip) => {
                let _ = write!(text, "clip={}", format_rect(clip.rect));
                if clip.corner_radii.has_any_radius() {
                    let corner_radii = clip.corner_radii;
                    let _ = write!(
                        text,
                        " radii=({},{},{},{})",
                        corner_radii.top_left.horizontal_radius,
                        corner_radii.top_right.horizontal_radius,
                        corner_radii.bottom_right.horizontal_radius,
                        corner_radii.bottom_left.horizontal_radius
                    );
                }
                if clip.mode == ClipMode::Difference {
                    text.push_str(" mode=difference");
                }
            }
            ClipNodeData::Path(clip_path) => {
                let svg_path = clip_path.path.to_svg_string();
                let has_curves_with_host_dependent_control_points = svg_path.contains('Q') || svg_path.contains('C');
                if has_curves_with_host_dependent_control_points {
                    let command_count = svg_path
                        .chars()
                        .filter(|code_point| matches!(code_point, 'M' | 'L' | 'Q' | 'C' | 'Z'))
                        .count();
                    let _ = write!(
                        text,
                        "clip_path=[bounds: {}, curved path: {} commands]",
                        format_int_rect_components(clip_path.bounding_rect),
                        command_count
                    );
                } else {
                    let _ = write!(
                        text,
                        "clip_path=[bounds: {}, path: {}]",
                        format_int_rect_components(clip_path.bounding_rect),
                        svg_path
                    );
                }
            }
            ClipNodeData::Dead => text.push_str("tombstone"),
        }
        text
    }

    pub fn dump_effect_node(&self, index: EffectNodeIndex) -> String {
        let mut text = String::new();
        match &self.effect_nodes[index.0 as usize].data {
            EffectNodeData::Effects(effects) => {
                text.push_str("effects=[");
                let mut has_content = false;
                if effects.opacity < 1.0 {
                    let _ = write!(text, "opacity={}", format_float_like_ak(effects.opacity));
                    has_content = true;
                }
                if effects.blend_mode != CompositingAndBlendingOperator::Normal {
                    if has_content {
                        text.push(' ');
                    }
                    let _ = write!(text, "blend_mode={}", effects.blend_mode as i32);
                    has_content = true;
                }
                if effects.filter.is_some() {
                    if has_content {
                        text.push(' ');
                    }
                    text.push_str("filter");
                    has_content = true;
                }
                if let Some(backdrop_filter) = &effects.backdrop_filter {
                    if has_content {
                        text.push(' ');
                    }
                    let _ = write!(
                        text,
                        "backdrop-filter=[{}]",
                        format_int_rect_components(backdrop_filter.region)
                    );
                }
                text.push(']');
            }
            EffectNodeData::Mask(mask) => {
                let kind = if mask.kind == MaskKind::Alpha {
                    "alpha"
                } else {
                    "luminance"
                };
                let origin = match mask.origin {
                    MaskLayerOrigin::CssMaskLayers => "css-mask-layers",
                    MaskLayerOrigin::SvgMask => "svg-mask",
                    MaskLayerOrigin::SvgClip => "svg-clip",
                };
                let _ = write!(
                    text,
                    "mask=[{}] kind={} origin={}",
                    format_int_rect_components(mask.rect),
                    kind,
                    origin
                );
            }
            EffectNodeData::BackgroundColorAnimation => text.push_str("background-color-animation"),
            EffectNodeData::Dead => text.push_str("tombstone"),
        }
        text
    }
}

impl VisualContextTree {
    // The nodes the runs record under, each tree in first-seen order. An effect's output clip
    // chain counts as reachable, since replay enters it before the effect.
    pub fn dump_nodes_reachable_from_runs(
        &self,
        command_runs: &[DisplayListCommandRun],
        mut owner_label: impl FnMut(SlotKind, u32) -> Option<String>,
    ) -> String {
        let effect_clips = crate::painting::display_list::effect_clip_plan::EffectClipPlan::new(self, command_runs)
            .expect("dumped command runs reference live visual context nodes");
        let mut visited_spatial_nodes: HashSet<u32> = HashSet::new();
        let mut visited_clip_nodes: HashSet<u32> = HashSet::new();
        let mut visited_effect_nodes: HashSet<u32> = HashSet::new();
        let mut spatial_children: HashMap<u32, Vec<u32>> = HashMap::new();
        let mut clip_children: HashMap<u32, Vec<u32>> = HashMap::new();
        let mut clip_roots: Vec<u32> = Vec::new();
        let mut effect_children: HashMap<u32, Vec<u32>> = HashMap::new();
        let mut effect_roots: Vec<u32> = Vec::new();

        let mut visit_clip_chain = |mut clip: ClipNodeIndex| {
            while !clip.is_none() && visited_clip_nodes.insert(clip.0) {
                let parent = self.clip_nodes[clip.0 as usize].parent;
                if parent.is_none() {
                    clip_roots.push(clip.0);
                } else {
                    clip_children.entry(parent.0).or_default().push(clip.0);
                }
                clip = parent;
            }
        };
        for run in command_runs {
            let mut spatial = run.context.spatial;
            while visited_spatial_nodes.insert(spatial.0) {
                if spatial == VISUAL_VIEWPORT_NODE_INDEX {
                    break;
                }
                let parent = self.spatial_nodes[spatial.0 as usize].parent;
                spatial_children.entry(parent.0).or_default().push(spatial.0);
                spatial = parent;
            }
            visit_clip_chain(run.context.clip);
            let mut effect = run.context.effect;
            while !effect.is_none() && visited_effect_nodes.insert(effect.0) {
                let node = &self.effect_nodes[effect.0 as usize];
                visit_clip_chain(effect_clips.output_clip(effect));
                if node.parent.is_none() {
                    effect_roots.push(effect.0);
                } else {
                    effect_children.entry(node.parent.0).or_default().push(effect.0);
                }
                effect = node.parent;
            }
        }

        let mut text = String::from("AccumulatedVisualContext Tree:\n");
        let mut append_owner = |text: &mut String, kind: SlotKind, index: u32| {
            if let Some(label) = owner_label(kind, index) {
                let _ = write!(text, " ({label})");
            }
            text.push('\n');
        };

        fn dump_subtree(
            text: &mut String,
            children: &HashMap<u32, Vec<u32>>,
            node_line: &impl Fn(u32) -> String,
            append_owner: &mut impl FnMut(&mut String, u32),
            node_index: u32,
            indent: usize,
        ) {
            text.push_str(&" ".repeat(indent * 2));
            text.push_str(&node_line(node_index));
            append_owner(text, node_index);
            if let Some(child_indices) = children.get(&node_index) {
                for child in child_indices {
                    dump_subtree(text, children, node_line, append_owner, *child, indent + 1);
                }
            }
        }

        text.push_str("  spatial:\n");
        dump_subtree(
            &mut text,
            &spatial_children,
            &|index: u32| format!("[s{index}] {}", self.dump_spatial_node(SpatialNodeIndex(index))),
            &mut |text: &mut String, index: u32| append_owner(text, SlotKind::Spatial, index),
            VISUAL_VIEWPORT_NODE_INDEX.0,
            2,
        );
        if !clip_roots.is_empty() {
            text.push_str("  clips:\n");
            for root in clip_roots {
                dump_subtree(
                    &mut text,
                    &clip_children,
                    &|index: u32| {
                        format!(
                            "[c{index} in s{}] {}",
                            self.clip_nodes[index as usize].spatial.0,
                            self.dump_clip_node(ClipNodeIndex(index))
                        )
                    },
                    &mut |text: &mut String, index: u32| append_owner(text, SlotKind::Clip, index),
                    root,
                    2,
                );
            }
        }
        if !effect_roots.is_empty() {
            text.push_str("  effects:\n");
            for root in effect_roots {
                dump_subtree(
                    &mut text,
                    &effect_children,
                    &|index: u32| {
                        let node = &self.effect_nodes[index as usize];
                        let resolved_clip = effect_clips.output_clip(EffectNodeIndex(index));
                        let output_clip = if resolved_clip.is_none() {
                            String::new()
                        } else {
                            format!(" out=c{}", resolved_clip.0)
                        };
                        format!(
                            "[e{index} in s{}{output_clip}] {}",
                            node.spatial.0,
                            self.dump_effect_node(EffectNodeIndex(index))
                        )
                    },
                    &mut |text: &mut String, index: u32| append_owner(text, SlotKind::Effect, index),
                    root,
                    2,
                );
            }
        }
        text
    }
}

#[cfg(test)]
mod node_dump_tests {
    use crate::layout::node_data::NodeSlotId;
    use crate::painting::visual_context::{
        AnchorScrollShift, BackfaceVisibilityData, ClipData, ClipMode, ClipNodeData, ClipNodeIndex, EffectNodeData,
        EffectNodeIndex, EffectsData, MaskData, MaskLayerOrigin, PerspectiveData, ScrollData, SpatialData, StickyData,
        TransformData, TransformDataRole, VISUAL_VIEWPORT_NODE_INDEX, VisualContextTree,
        scroll_state::NO_SCROLL_STATE_SLOT,
    };
    use libgfx_rust::{
        CompositingAndBlendingOperator, CornerRadii, FloatMatrix4x4, FloatPoint, FloatRect, FloatSize, IntRect,
        MaskKind, translation_matrix,
    };

    fn tree() -> VisualContextTree {
        VisualContextTree::create(TransformData {
            matrix: FloatMatrix4x4::identity(),
            origin: FloatPoint::default(),
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
        })
    }

    #[test]
    fn spatial_nodes_dump_like_the_display_list_expectations() {
        let mut tree = tree();
        let transformed = tree.append_spatial(
            SpatialData::Transform(TransformData {
                matrix: FloatMatrix4x4 {
                    elements: [
                        [0.8660254, 0.0, 0.0, 0.0],
                        [0.0, 1.0, 0.0, 0.0],
                        [0.0, 0.0, 1.0, 0.0],
                        [0.0, 0.0, 0.0, 1.0],
                    ],
                },
                origin: FloatPoint { x: 58.0, y: 58.0 },
                sorting_context_root_index: None,
                flattens_inherited_transform: false,
                role: TransformDataRole::CssTransform,
                synthetic_plane: false,
                establishes_sorting_context: false,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let svg_viewport = tree.append_spatial(
            SpatialData::Transform(TransformData {
                matrix: translation_matrix(1.0, 0.0, 0.0),
                origin: FloatPoint::default(),
                sorting_context_root_index: None,
                flattens_inherited_transform: false,
                role: TransformDataRole::SvgViewportTransform,
                synthetic_plane: false,
                establishes_sorting_context: false,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let scroll_node = tree.append_spatial(
            SpatialData::Scroll(ScrollData {
                state_slot: NO_SCROLL_STATE_SLOT,
                owner_paintable: NodeSlotId::INVALID,
                registry_parent_node: VISUAL_VIEWPORT_NODE_INDEX,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let outer_sticky = tree.append_spatial(
            SpatialData::Sticky(StickyData::unconstrained(
                scroll_node,
                None,
                NO_SCROLL_STATE_SLOT,
                NodeSlotId::INVALID,
                scroll_node,
            )),
            scroll_node,
        );
        let inner_sticky = tree.append_spatial(
            SpatialData::Sticky(StickyData {
                scroller: scroll_node,
                parent_sticky: Some(outer_sticky),
                position_relative_to_scroller: FloatPoint::default(),
                border_box_size: FloatSize {
                    width: 800.0,
                    height: 16.0,
                },
                scrollport_size: FloatSize {
                    width: 800.0,
                    height: 600.0,
                },
                containing_block_region: FloatRect::new(0.0, 0.0, 800.0, 16.0),
                needs_parent_offset_adjustment: true,
                inset_top: Some(20.0),
                inset_right: None,
                inset_bottom: Some(1.5),
                inset_left: None,
                state_slot: NO_SCROLL_STATE_SLOT,
                owner_paintable: NodeSlotId::INVALID,
                registry_parent_node: scroll_node,
            }),
            outer_sticky,
        );
        let perspective = tree.append_spatial(
            SpatialData::Perspective(PerspectiveData {
                matrix: FloatMatrix4x4::identity(),
                flattens_inherited_transform: false,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let backface = tree.append_spatial(
            SpatialData::BackfaceVisibility(BackfaceVisibilityData {
                plane_root_index: transformed,
                flattens_inherited_transform: false,
            }),
            transformed,
        );
        let anchor_shift = tree.append_spatial(
            SpatialData::AnchorScrollShift(AnchorScrollShift {
                scroll_node_index: scroll_node,
                negate: true,
                compensate_horizontal_scroll: false,
                compensate_vertical_scroll: true,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );

        assert_eq!(
            tree.dump_spatial_node(VISUAL_VIEWPORT_NODE_INDEX),
            "transform=[1,0,0,1,0,0] origin=(0,0)"
        );
        assert_eq!(
            tree.dump_spatial_node(transformed),
            "transform=[0.8660254,0,0,1,0,0] origin=(58,58)"
        );
        assert_eq!(
            tree.dump_spatial_node(svg_viewport),
            "svg-viewport-transform=[1,0,0,1,1,0] origin=(0,0)"
        );
        assert_eq!(tree.dump_spatial_node(scroll_node), "scroll");
        assert_eq!(
            tree.dump_spatial_node(outer_sticky),
            "sticky scroller=s3 position_relative_to_scroller=[0,0] border_box_size=[0x0] scrollport_size=[0x0] containing_block_region=[0,0 0x0] needs_parent_offset_adjustment=false insets=[]"
        );
        assert_eq!(
            tree.dump_spatial_node(inner_sticky),
            "sticky scroller=s3 parent_sticky=s4 position_relative_to_scroller=[0,0] border_box_size=[800x16] scrollport_size=[800x600] containing_block_region=[0,0 800x16] needs_parent_offset_adjustment=true insets=[top=20, bottom=1.5]"
        );
        assert_eq!(tree.dump_spatial_node(perspective), "perspective");
        assert_eq!(tree.dump_spatial_node(backface), "backface-hidden plane_root=s1");
        assert_eq!(
            tree.dump_spatial_node(anchor_shift),
            "anchor_scroll_shift(node_index=s3, negate, no-x)"
        );
    }

    #[test]
    fn clip_and_effect_nodes_dump_like_the_display_list_expectations() {
        let mut tree = tree();
        let plain_clip = tree.append_clip(
            ClipNodeData::Rect(ClipData {
                rect: FloatRect::new(11.0, 10.0, 100.0, 16.0),
                corner_radii: CornerRadii::default(),
                mode: ClipMode::Intersect,
            }),
            ClipNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let rounded_difference_clip = tree.append_clip(
            ClipNodeData::Rect(ClipData {
                rect: FloatRect::new(0.5, 0.0, 10.0, 10.0),
                corner_radii: CornerRadii::uniform(3),
                mode: ClipMode::Difference,
            }),
            ClipNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let plain_effects = tree.append_effect(
            EffectNodeData::Effects(EffectsData {
                opacity: 1.0,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
                backdrop_filter: None,
            }),
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );
        let full_effects = tree.append_effect(
            EffectNodeData::Effects(EffectsData {
                opacity: 0.5,
                blend_mode: CompositingAndBlendingOperator::Multiply,
                filter: Some(std::rc::Rc::new(vec![1, 2, 3])),
                backdrop_filter: None,
            }),
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );
        let mask = tree.append_effect(
            EffectNodeData::Mask(MaskData {
                rect: IntRect::new(1, 2, 30, 40),
                kind: MaskKind::Luminance,
                origin: MaskLayerOrigin::SvgMask,
            }),
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );
        let marker = tree.append_effect(
            EffectNodeData::BackgroundColorAnimation,
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );

        assert_eq!(tree.dump_clip_node(plain_clip), "clip=[11,10 100x16]");
        assert_eq!(
            tree.dump_clip_node(rounded_difference_clip),
            "clip=[0.5,0 10x10] radii=(3,3,3,3) mode=difference"
        );
        assert_eq!(tree.dump_effect_node(plain_effects), "effects=[]");
        assert_eq!(
            tree.dump_effect_node(full_effects),
            format!(
                "effects=[opacity=0.5 blend_mode={} filter]",
                CompositingAndBlendingOperator::Multiply as i32
            )
        );
        assert_eq!(
            tree.dump_effect_node(mask),
            "mask=[1,2 30x40] kind=luminance origin=svg-mask"
        );
        assert_eq!(tree.dump_effect_node(marker), "background-color-animation");
    }
}

#[cfg(test)]
mod section_dump_tests {
    use super::SlotKind;
    use crate::layout::node_data::NodeSlotId;
    use crate::painting::display_list::commands::{ContextRef, DisplayListCommandRun, SpatialNodeIndex};
    use crate::painting::visual_context::{
        ClipData, ClipMode, ClipNodeData, ClipNodeIndex, EffectNodeData, EffectNodeIndex, EffectsData, ScrollData,
        SpatialData, TransformData, TransformDataRole, VISUAL_VIEWPORT_NODE_INDEX, VisualContextTree,
        scroll_state::NO_SCROLL_STATE_SLOT,
    };
    use libgfx_rust::{CompositingAndBlendingOperator, CornerRadii, FloatMatrix4x4, FloatPoint, FloatRect, IntRect};

    fn run(spatial: SpatialNodeIndex, context: ContextRef) -> DisplayListCommandRun {
        DisplayListCommandRun {
            offset: 0,
            size: 0,
            context: ContextRef { spatial, ..context },
            ink_bounds: IntRect::default(),
            has_unbounded_draw: false,
            has_compositor_metadata: false,
        }
    }

    #[test]
    fn the_dump_lists_reachable_nodes_in_first_seen_order_with_owner_labels() {
        let mut tree = VisualContextTree::create(TransformData {
            matrix: FloatMatrix4x4::identity(),
            origin: FloatPoint::default(),
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
        });
        let scroll_node = tree.append_spatial(
            SpatialData::Scroll(ScrollData {
                state_slot: NO_SCROLL_STATE_SLOT,
                owner_paintable: NodeSlotId::INVALID,
                registry_parent_node: VISUAL_VIEWPORT_NODE_INDEX,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let unreachable_node = tree.append_spatial(
            SpatialData::Scroll(ScrollData {
                state_slot: NO_SCROLL_STATE_SLOT,
                owner_paintable: NodeSlotId::INVALID,
                registry_parent_node: VISUAL_VIEWPORT_NODE_INDEX,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let outer_clip = tree.append_clip(
            ClipNodeData::Rect(ClipData {
                rect: FloatRect::new(1.0, 2.0, 3.0, 4.0),
                corner_radii: CornerRadii::default(),
                mode: ClipMode::Intersect,
            }),
            ClipNodeIndex::NONE,
            scroll_node,
        );
        let effect = tree.append_effect(
            EffectNodeData::Effects(EffectsData {
                opacity: 0.5,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
                backdrop_filter: None,
            }),
            EffectNodeIndex::NONE,
            scroll_node,
            outer_clip,
        );
        let inner_clip = tree.append_clip(
            ClipNodeData::rect_clip(FloatRect::new(5.0, 6.0, 7.0, 8.0)),
            outer_clip,
            scroll_node,
        );
        let unreachable_clip = tree.append_clip(
            ClipNodeData::rect_clip(FloatRect::new(9.0, 9.0, 9.0, 9.0)),
            ClipNodeIndex::NONE,
            scroll_node,
        );
        let inner_context = ContextRef {
            clip: inner_clip,
            effect,
            ..ContextRef::default()
        };
        let escaping_context = ContextRef {
            clip: outer_clip,
            effect,
            ..ContextRef::default()
        };
        let _ = (unreachable_node, unreachable_clip);
        let runs = [
            run(scroll_node, inner_context),
            run(scroll_node, escaping_context),
            run(VISUAL_VIEWPORT_NODE_INDEX, ContextRef::default()),
        ];
        let text = tree.dump_nodes_reachable_from_runs(&runs, |kind, index| match (kind, index) {
            (SlotKind::Spatial, 1) => Some("BlockContainer<DIV>#scroller".to_string()),
            (SlotKind::Clip, 0) => Some("BlockContainer<DIV>#scroller".to_string()),
            (SlotKind::Effect, 0) => Some("BlockContainer<DIV>#faded".to_string()),
            _ => None,
        });
        assert_eq!(
            text,
            "AccumulatedVisualContext Tree:\n  spatial:\n    [s0] transform=[1,0,0,1,0,0] origin=(0,0)\n      [s1] scroll (BlockContainer<DIV>#scroller)\n  clips:\n    [c0 in s1] clip=[1,2 3x4] (BlockContainer<DIV>#scroller)\n      [c1 in s1] clip=[5,6 7x8]\n  effects:\n    [e0 in s1 out=c0] effects=[opacity=0.5] (BlockContainer<DIV>#faded)\n"
        );
    }
}
