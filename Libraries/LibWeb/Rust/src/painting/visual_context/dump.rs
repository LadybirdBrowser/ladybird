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
use crate::painting::display_list::dump_format::{
    DumpNames, path_description, push_clip_radii, push_corner_radii, push_filter_bytes, push_matrix,
};
use crate::painting::dump::{
    format_float_like_ak, push_float_point, push_float_rect, push_float_size, push_int_rect_components,
};
use libgfx_rust::{
    CompositingAndBlendingOperator, FloatMatrix4x4, FloatPoint, FloatRect, FloatSize, IntRect, MaskKind,
};
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

fn format_int_rect_components(rect: IntRect) -> String {
    let mut text = String::new();
    push_int_rect_components(&mut text, rect);
    text
}

// A 2D affine matrix as its six components, any other as all sixteen in row order.
fn push_transform_matrix(text: &mut String, keyword: &str, matrix: &FloatMatrix4x4) {
    if matrix.is_2d_affine() {
        let elements = &matrix.elements;
        let _ = write!(
            text,
            "{keyword}=[{},{},{},{},{},{}]",
            format_float_like_ak(elements[0][0]),
            format_float_like_ak(elements[0][1]),
            format_float_like_ak(elements[1][0]),
            format_float_like_ak(elements[1][1]),
            format_float_like_ak(elements[0][3]),
            format_float_like_ak(elements[1][3]),
        );
    } else {
        let _ = write!(text, "{keyword}3d=");
        push_matrix(text, matrix);
    }
}

/// The debug dumps the layout and display list tests compare against.
pub trait VisualContextTreeDump {
    fn dump_spatial_node(&self, index: SpatialNodeIndex) -> String;
    fn dump_clip_node(&self, index: ClipNodeIndex) -> String;
    fn dump_effect_node(&self, index: EffectNodeIndex) -> String;
    fn describe_spatial_node(&self, index: SpatialNodeIndex, names: &mut DumpNames) -> String;
    fn describe_clip_node(&self, index: ClipNodeIndex, names: &mut DumpNames) -> String;
    fn describe_effect_node(&self, index: EffectNodeIndex, names: &mut DumpNames) -> String;
    // The nodes the runs record under, each tree in first-seen order. An effect's output clip
    // chain counts as reachable, since replay enters it before the effect.
    fn reachable_nodes_in_print_order<'a>(
        &self,
        command_runs: impl IntoIterator<Item = &'a DisplayListCommandRun> + Clone,
        names: &mut DumpNames,
        owner_label: impl FnMut(SlotKind, u32) -> Option<String>,
    ) -> Vec<DumpedNode>;
}

impl VisualContextTreeDump for VisualContextTree {
    fn dump_spatial_node(&self, index: SpatialNodeIndex) -> String {
        self.describe_spatial_node(index, &mut DumpNames::default())
    }

    fn dump_clip_node(&self, index: ClipNodeIndex) -> String {
        self.describe_clip_node(index, &mut DumpNames::default())
    }

    fn dump_effect_node(&self, index: EffectNodeIndex) -> String {
        self.describe_effect_node(index, &mut DumpNames::default())
    }

    fn describe_spatial_node(&self, index: SpatialNodeIndex, names: &mut DumpNames) -> String {
        let mut text = String::new();
        // A node flattens what it inherits unless its box preserves 3D. The root inherits nothing.
        let inherits_nothing = index == VISUAL_VIEWPORT_NODE_INDEX;
        let flattens_marker = |flattens_inherited_transform: bool| {
            if flattens_inherited_transform || inherits_nothing {
                ""
            } else {
                " flattens_inherited_transform=false"
            }
        };
        match &self.spatial_nodes[index.0 as usize].data {
            SpatialData::Perspective(perspective) => {
                text.push_str("perspective=");
                push_matrix(&mut text, &perspective.matrix);
                text.push_str(flattens_marker(perspective.flattens_inherited_transform));
            }
            SpatialData::BackfaceVisibility(backface) => {
                let _ = write!(
                    text,
                    "backface-hidden plane_root={}{}",
                    names.spatial(backface.plane_root_index),
                    flattens_marker(backface.flattens_inherited_transform)
                );
            }
            SpatialData::Scroll(_) => text.push_str("scroll"),
            SpatialData::Sticky(sticky) => {
                let _ = write!(text, "sticky scroller={}", names.spatial(sticky.scroller));
                if let Some(parent_sticky) = sticky.parent_sticky {
                    let _ = write!(text, " parent_sticky={}", names.spatial(parent_sticky));
                }
                let _ = write!(
                    text,
                    " position_relative_to_scroller={} border_box_origin={} border_box_size={} scrollport_size={} containing_block_region={} needs_parent_offset_adjustment={}",
                    format_point(sticky.position_relative_to_scroller),
                    format_point(sticky.border_box_origin),
                    format_size(sticky.border_box_size),
                    format_size(sticky.scrollport_size),
                    format_rect(sticky.containing_block_region),
                    sticky.needs_parent_offset_adjustment
                );
                if !sticky.non_scrolling_scroll_container.is_invalid() {
                    text.push_str(" non_scrolling_scroll_container");
                }
                text.push_str(" insets=[");
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
                let keyword = match transform.role {
                    TransformDataRole::CssTransform => "transform",
                    TransformDataRole::SvgViewportTransform => "svg-viewport-transform",
                };
                push_transform_matrix(&mut text, keyword, &transform.matrix);
                let _ = write!(
                    text,
                    " origin=({},{})",
                    format_float_like_ak(transform.origin.x),
                    format_float_like_ak(transform.origin.y)
                );
                if let Some(sorting_context_root) = transform.sorting_context_root_index {
                    let _ = write!(text, " sorting_context_root={}", names.spatial(sorting_context_root));
                }
                text.push_str(flattens_marker(transform.flattens_inherited_transform));
                if transform.synthetic_plane {
                    text.push_str(" synthetic_plane");
                }
                if transform.establishes_sorting_context {
                    text.push_str(" establishes_sorting_context");
                }
                if !transform.bounds.is_empty() {
                    let _ = write!(text, " bounds={}", format_rect(transform.bounds));
                }
            }
            SpatialData::AnchorScrollShift(shift) => {
                let _ = write!(
                    text,
                    "anchor_scroll_shift(node_index={}{}{}{})",
                    names.spatial(shift.scroll_node_index),
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

    fn describe_clip_node(&self, index: ClipNodeIndex, _names: &mut DumpNames) -> String {
        let mut text = String::new();
        match &self.clip_nodes[index.0 as usize].data {
            ClipNodeData::Rect(clip) => {
                let _ = write!(text, "clip={}", format_rect(clip.rect));
                if clip.corner_radii.has_any_radius() {
                    text.push_str(" radii=");
                    push_clip_radii(&mut text, clip.corner_radii);
                }
                if clip.mode == ClipMode::Difference {
                    text.push_str(" mode=difference");
                }
            }
            ClipNodeData::Path(clip_path) => {
                let _ = write!(
                    text,
                    "clip_path=[bounds: {}, {}]",
                    format_int_rect_components(clip_path.bounding_rect),
                    path_description(&clip_path.path)
                );
                if clip_path.fill_rule != libgfx_rust::WindingRule::default() {
                    let _ = write!(text, " fill_rule={:?}", clip_path.fill_rule);
                }
            }
            ClipNodeData::Dead => text.push_str("tombstone"),
        }
        text
    }

    fn describe_effect_node(&self, index: EffectNodeIndex, names: &mut DumpNames) -> String {
        let mut text = String::new();
        match &self.effect_nodes[index.0 as usize].data {
            EffectNodeData::Effects(effects) => {
                text.push_str("effects=[");
                let mut separator = "";
                if effects.isolation {
                    text.push_str("isolation");
                    separator = " ";
                }
                if effects.opacity < 1.0 {
                    let _ = write!(text, "{separator}opacity={}", format_float_like_ak(effects.opacity));
                    separator = " ";
                }
                if effects.blend_mode != CompositingAndBlendingOperator::Normal {
                    let _ = write!(text, "{separator}blend_mode={:?}", effects.blend_mode);
                    separator = " ";
                }
                if let Some(filter) = &effects.filter {
                    let _ = write!(text, "{separator}filter=[");
                    push_filter_bytes(&mut text, filter, names);
                    text.push(']');
                    separator = " ";
                }
                if let Some(backdrop_filter) = &effects.backdrop_filter {
                    let _ = write!(
                        text,
                        "{separator}backdrop-filter=[region=[{}]",
                        format_int_rect_components(backdrop_filter.region)
                    );
                    if backdrop_filter.corner_radii.has_any_radius() {
                        text.push_str(" corner_radii=");
                        push_corner_radii(&mut text, backdrop_filter.corner_radii);
                    }
                    text.push_str(" filter=[");
                    push_filter_bytes(&mut text, &backdrop_filter.filter, names);
                    text.push_str("]]");
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
                    "mask=[{}] kind={} origin={}{}",
                    format_int_rect_components(mask.rect),
                    kind,
                    origin,
                    if mask.masks_backdrop { " masks-backdrop" } else { "" }
                );
            }
            EffectNodeData::BackgroundColorAnimation => text.push_str("background-color-animation"),
            EffectNodeData::Dead => text.push_str("tombstone"),
        }
        text
    }

    fn reachable_nodes_in_print_order<'a>(
        &self,
        command_runs: impl IntoIterator<Item = &'a DisplayListCommandRun> + Clone,
        names: &mut DumpNames,
        mut owner_label: impl FnMut(SlotKind, u32) -> Option<String>,
    ) -> Vec<DumpedNode> {
        let effect_clips =
            crate::painting::display_list::effect_clip_plan::EffectClipPlan::new(self, command_runs.clone())
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

        let mut nodes = Vec::new();
        let mut walk = |kind: SlotKind,
                        root: u32,
                        children: &HashMap<u32, Vec<u32>>,
                        names: &mut DumpNames,
                        nodes: &mut Vec<DumpedNode>| {
            let mut pending = vec![(root, 0)];
            while let Some((index, depth)) = pending.pop() {
                let mut node = match kind {
                    SlotKind::Spatial => {
                        let name = names.spatial(SpatialNodeIndex(index));
                        let parent = (index != VISUAL_VIEWPORT_NODE_INDEX.0)
                            .then(|| names.spatial(self.spatial_nodes[index as usize].parent));
                        DumpedNode {
                            kind,
                            index,
                            name,
                            depth,
                            parent,
                            space: None,
                            output_clip: None,
                            description: self.describe_spatial_node(SpatialNodeIndex(index), names),
                            owner: None,
                        }
                    }
                    SlotKind::Clip => {
                        let node = &self.clip_nodes[index as usize];
                        let name = names.clip(ClipNodeIndex(index));
                        let space = names.spatial(node.spatial);
                        let parent = (!node.parent.is_none()).then(|| names.clip(node.parent));
                        DumpedNode {
                            kind,
                            index,
                            name,
                            depth,
                            parent,
                            space: Some(space),
                            output_clip: None,
                            description: self.describe_clip_node(ClipNodeIndex(index), names),
                            owner: None,
                        }
                    }
                    SlotKind::Effect => {
                        let node = &self.effect_nodes[index as usize];
                        let name = names.effect(EffectNodeIndex(index));
                        let space = names.spatial(node.spatial);
                        let resolved_clip = effect_clips.output_clip(EffectNodeIndex(index));
                        let output_clip = (!resolved_clip.is_none()).then(|| names.clip(resolved_clip));
                        let parent = (!node.parent.is_none()).then(|| names.effect(node.parent));
                        DumpedNode {
                            kind,
                            index,
                            name,
                            depth,
                            parent,
                            space: Some(space),
                            output_clip,
                            description: self.describe_effect_node(EffectNodeIndex(index), names),
                            owner: None,
                        }
                    }
                };
                node.owner = owner_label(kind, index);
                nodes.push(node);
                if let Some(child_indices) = children.get(&index) {
                    pending.extend(child_indices.iter().rev().map(|&child| (child, depth + 1)));
                }
            }
        };
        walk(
            SlotKind::Spatial,
            VISUAL_VIEWPORT_NODE_INDEX.0,
            &spatial_children,
            names,
            &mut nodes,
        );
        for root in clip_roots {
            walk(SlotKind::Clip, root, &clip_children, names, &mut nodes);
        }
        for root in effect_roots {
            walk(SlotKind::Effect, root, &effect_children, names, &mut nodes);
        }
        nodes
    }
}

/// A node of the tree section of the display list dump, with the names the dump gives it and the nodes it names.
pub(crate) struct DumpedNode {
    pub(crate) kind: SlotKind,
    pub(crate) index: u32,
    pub(crate) name: String,
    pub(crate) depth: usize,
    pub(crate) parent: Option<String>,
    pub(crate) space: Option<String>,
    pub(crate) output_clip: Option<String>,
    pub(crate) description: String,
    pub(crate) owner: Option<String>,
}

pub(crate) fn tree_section_text(nodes: &[DumpedNode]) -> String {
    let mut text = String::from("AccumulatedVisualContext Tree:\n");
    let mut previous_kind = None;
    for node in nodes {
        if previous_kind != Some(node.kind) {
            text.push_str(match node.kind {
                SlotKind::Spatial => "  spatial:\n",
                SlotKind::Clip => "  clips:\n",
                SlotKind::Effect => "  effects:\n",
            });
            previous_kind = Some(node.kind);
        }
        text.push_str(&" ".repeat((node.depth + 2) * 2));
        let _ = write!(text, "[{}", node.name);
        if let Some(space) = &node.space {
            let _ = write!(text, " in {space}");
        }
        if let Some(output_clip) = &node.output_clip {
            let _ = write!(text, " out={output_clip}");
        }
        let _ = write!(text, "] {}", node.description);
        if let Some(owner) = &node.owner {
            let _ = write!(text, " ({owner})");
        }
        text.push('\n');
    }
    text
}

#[cfg(test)]
mod node_dump_tests {
    use super::VisualContextTreeDump;
    use crate::layout::node_data::NodeSlotId;
    use crate::painting::visual_context::{
        AnchorScrollShift, BackdropFilterData, BackfaceVisibilityData, ClipData, ClipMode, ClipNodeData, ClipNodeIndex,
        EffectNodeData, EffectNodeIndex, EffectsData, MaskData, MaskLayerOrigin, PerspectiveData, ScrollData,
        SpatialData, StickyData, TransformData, TransformDataRole, UniqueNodeId, VISUAL_VIEWPORT_NODE_INDEX,
        VisualContextTree, scroll_state::NO_SCROLL_STATE_SLOT,
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
            bounds: FloatRect::default(),
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
                bounds: FloatRect::default(),
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
                bounds: FloatRect::default(),
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let scroll_node = tree.append_spatial(
            SpatialData::Scroll(ScrollData {
                state_slot: NO_SCROLL_STATE_SLOT,
                owner_paintable: NodeSlotId::INVALID,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let outer_sticky = tree.append_spatial(
            SpatialData::Sticky(StickyData::unconstrained(
                scroll_node,
                None,
                NO_SCROLL_STATE_SLOT,
                NodeSlotId::INVALID,
            )),
            scroll_node,
        );
        let inner_sticky = tree.append_spatial(
            SpatialData::Sticky(StickyData {
                scroller: scroll_node,
                parent_sticky: Some(outer_sticky),
                position_relative_to_scroller: FloatPoint::default(),
                border_box_origin: FloatPoint::default(),
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
                non_scrolling_scroll_container: NodeSlotId::INVALID,
                inset_top: Some(20.0),
                inset_right: None,
                inset_bottom: Some(1.5),
                inset_left: None,
                box_node_id: UniqueNodeId(0),
                box_pseudo_element_type: 0,
                state_slot: NO_SCROLL_STATE_SLOT,
                owner_paintable: NodeSlotId::INVALID,
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
        let rotated_in_3d = tree.append_spatial(
            SpatialData::Transform(TransformData {
                matrix: FloatMatrix4x4 {
                    elements: [
                        [1.0, 0.0, 0.0, 0.0],
                        [0.0, 0.0, -1.0, 0.0],
                        [0.0, 1.0, 0.0, 0.0],
                        [0.0, 0.0, 0.0, 1.0],
                    ],
                },
                origin: FloatPoint::default(),
                sorting_context_root_index: Some(transformed),
                flattens_inherited_transform: true,
                role: TransformDataRole::CssTransform,
                synthetic_plane: true,
                establishes_sorting_context: true,
                bounds: FloatRect::new(0.0, 0.0, 10.0, 20.0),
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
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
            "transform=[0.8660254,0,0,1,0,0] origin=(58,58) flattens_inherited_transform=false"
        );
        assert_eq!(
            tree.dump_spatial_node(svg_viewport),
            "svg-viewport-transform=[1,0,0,1,1,0] origin=(0,0) flattens_inherited_transform=false"
        );
        assert_eq!(
            tree.dump_spatial_node(rotated_in_3d),
            "transform3d=[1,0,0,0,0,0,-1,0,0,1,0,0,0,0,0,1] origin=(0,0) sorting_context_root=s1 synthetic_plane establishes_sorting_context bounds=[0,0 10x20]"
        );
        assert_eq!(tree.dump_spatial_node(scroll_node), "scroll");
        assert_eq!(
            tree.dump_spatial_node(outer_sticky),
            "sticky scroller=s3 position_relative_to_scroller=[0,0] border_box_origin=[0,0] border_box_size=[0x0] scrollport_size=[0x0] containing_block_region=[0,0 0x0] needs_parent_offset_adjustment=false insets=[]"
        );
        assert_eq!(
            tree.dump_spatial_node(inner_sticky),
            "sticky scroller=s3 parent_sticky=s4 position_relative_to_scroller=[0,0] border_box_origin=[0,0] border_box_size=[800x16] scrollport_size=[800x600] containing_block_region=[0,0 800x16] needs_parent_offset_adjustment=true insets=[top=20, bottom=1.5]"
        );
        assert_eq!(
            tree.dump_spatial_node(perspective),
            "perspective=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1] flattens_inherited_transform=false"
        );
        assert_eq!(
            tree.dump_spatial_node(backface),
            "backface-hidden plane_root=s1 flattens_inherited_transform=false"
        );
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
        let full_effects = tree.append_effect(
            EffectNodeData::Effects(EffectsData {
                isolation: false,
                opacity: 0.5,
                blend_mode: CompositingAndBlendingOperator::Multiply,
                filter: Some(std::sync::Arc::new(vec![1, 2, 3])),
                backdrop_filter: None,
            }),
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );
        let isolated_blurred_backdrop = tree.append_effect(
            EffectNodeData::Effects(EffectsData {
                isolation: true,
                opacity: 1.0,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
                backdrop_filter: Some(BackdropFilterData {
                    filter: std::sync::Arc::new(
                        libgfx_rust::filter::Filter::compose(
                            libgfx_rust::filter::Filter::color(libgfx_rust::ColorFilterType::Invert, 1.0, None),
                            libgfx_rust::filter::Filter::blur(4.0, 4.0, None),
                        )
                        .serialize(),
                    ),
                    region: IntRect::new(1, 2, 30, 40),
                    corner_radii: CornerRadii::uniform(3),
                }),
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
                masks_backdrop: false,
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
            "effects=[opacity=0.5 blend_mode=Multiply filter=[unreadable 3 bytes]]"
        );
        assert_eq!(
            tree.dump_effect_node(isolated_blurred_backdrop),
            "effects=[isolation backdrop-filter=[region=[1,2 30x40] corner_radii=[3x3,3x3,3x3,3x3] filter=[blur(4,4) Invert(1)]]]"
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
    use super::{DumpedNode, SlotKind, VisualContextTreeDump, tree_section_text};
    use crate::layout::node_data::NodeSlotId;
    use crate::painting::display_list::commands::{ContextRef, DisplayListCommandRun, SpatialNodeIndex};
    use crate::painting::display_list::dump_format::DumpNames;
    use crate::painting::visual_context::{
        ClipData, ClipMode, ClipNodeData, ClipNodeIndex, EffectNodeData, EffectNodeIndex, EffectsData, ScrollData,
        SpatialData, TransformData, TransformDataRole, VISUAL_VIEWPORT_NODE_INDEX, VisualContextTree,
        scroll_state::NO_SCROLL_STATE_SLOT,
    };
    use libgfx_rust::{CompositingAndBlendingOperator, CornerRadii, FloatMatrix4x4, FloatPoint, FloatRect, IntRect};

    fn tree_section_text_of(
        tree: &VisualContextTree,
        runs: &[DisplayListCommandRun],
        names: &mut DumpNames,
        owner_label: impl FnMut(SlotKind, u32) -> Option<String>,
    ) -> String {
        let nodes: Vec<DumpedNode> = tree.reachable_nodes_in_print_order(runs, names, owner_label);
        tree_section_text(&nodes)
    }

    fn run(spatial: SpatialNodeIndex, context: ContextRef) -> DisplayListCommandRun {
        DisplayListCommandRun {
            offset: 0,
            size: 0,
            context: ContextRef { spatial, ..context },
            ink_bounds: IntRect::default(),
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
            bounds: FloatRect::default(),
        });
        let scroll_node = tree.append_spatial(
            SpatialData::Scroll(ScrollData {
                state_slot: NO_SCROLL_STATE_SLOT,
                owner_paintable: NodeSlotId::INVALID,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let unreachable_node = tree.append_spatial(
            SpatialData::Scroll(ScrollData {
                state_slot: NO_SCROLL_STATE_SLOT,
                owner_paintable: NodeSlotId::INVALID,
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
                isolation: false,
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
        let mut names = DumpNames::default();
        let text = tree_section_text_of(&tree, &runs, &mut names, |kind, index| match (kind, index) {
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

    #[test]
    fn nodes_are_numbered_in_the_order_the_dump_first_prints_them() {
        let mut tree = VisualContextTree::create(TransformData {
            matrix: FloatMatrix4x4::identity(),
            origin: FloatPoint::default(),
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
            bounds: FloatRect::default(),
        });
        let scroll_node = |tree: &mut VisualContextTree| {
            tree.append_spatial(
                SpatialData::Scroll(ScrollData {
                    state_slot: NO_SCROLL_STATE_SLOT,
                    owner_paintable: NodeSlotId::INVALID,
                }),
                VISUAL_VIEWPORT_NODE_INDEX,
            )
        };
        let _unreachable_scroll_node = scroll_node(&mut tree);
        let scroller = scroll_node(&mut tree);
        let _unreachable_clip = tree.append_clip(
            ClipNodeData::rect_clip(FloatRect::new(9.0, 9.0, 9.0, 9.0)),
            ClipNodeIndex::NONE,
            scroller,
        );
        let clip = tree.append_clip(
            ClipNodeData::rect_clip(FloatRect::new(1.0, 2.0, 3.0, 4.0)),
            ClipNodeIndex::NONE,
            scroller,
        );
        let runs = [run(
            scroller,
            ContextRef {
                clip,
                ..ContextRef::default()
            },
        )];
        let dump = |names: &mut DumpNames| tree_section_text_of(&tree, &runs, names, |_, _| None);
        assert_eq!(
            dump(&mut DumpNames::numbering_nodes_in_print_order()),
            "AccumulatedVisualContext Tree:\n  spatial:\n    [s0] transform=[1,0,0,1,0,0] origin=(0,0)\n      [s1] scroll\n  clips:\n    [c0 in s1] clip=[1,2 3x4]\n"
        );
        assert_eq!(
            dump(&mut DumpNames::default()),
            "AccumulatedVisualContext Tree:\n  spatial:\n    [s0] transform=[1,0,0,1,0,0] origin=(0,0)\n      [s2] scroll\n  clips:\n    [c1 in s2] clip=[1,2 3x4]\n"
        );
    }
}
