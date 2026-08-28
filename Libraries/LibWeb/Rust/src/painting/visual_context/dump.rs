/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::{
    ClipMode, FrameData, FrameNodeIndex, MaskLayerOrigin, SpatialData, SpatialNodeIndex, TransformDataRole,
    VISUAL_VIEWPORT_NODE_INDEX, VisualContextTree,
};
use crate::painting::display_list::commands::DisplayListCommandRun;
use crate::painting::dump::format_float_like_ak;
use libgfx_rust::{CompositingAndBlendingOperator, FloatPoint, FloatRect, FloatSize, IntRect, MaskKind};
use std::collections::{HashMap, HashSet};
use std::fmt::Write;

fn format_point(point: FloatPoint) -> String {
    format!("[{},{}]", format_float_like_ak(point.x), format_float_like_ak(point.y))
}

fn format_size(size: FloatSize) -> String {
    format!(
        "[{}x{}]",
        format_float_like_ak(size.width),
        format_float_like_ak(size.height)
    )
}

fn format_rect(rect: FloatRect) -> String {
    format!(
        "[{},{} {}x{}]",
        format_float_like_ak(rect.x),
        format_float_like_ak(rect.y),
        format_float_like_ak(rect.width),
        format_float_like_ak(rect.height)
    )
}

fn format_spatial_node_index(index: SpatialNodeIndex) -> String {
    format!("s{}", index.0)
}

fn format_int_rect_components(rect: IntRect) -> String {
    format!("{},{} {}x{}", rect.x, rect.y, rect.width, rect.height)
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

    pub fn dump_frame_node(&self, index: FrameNodeIndex) -> String {
        let mut text = String::new();
        match &self.frame_nodes[index.0 as usize].data {
            FrameData::Clip(clip) => {
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
            FrameData::ClipPath(clip_path) => {
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
            FrameData::Effects(effects) => {
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
                }
                text.push(']');
            }
            FrameData::Mask(mask) => {
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
            FrameData::Dead => text.push_str("tombstone"),
        }
        text
    }
}

impl VisualContextTree {
    pub fn dump_nodes_reachable_from_runs(
        &self,
        command_runs: &[DisplayListCommandRun],
        mut owner_label: impl FnMut(bool, u32) -> Option<String>,
    ) -> String {
        let mut visited_spatial_nodes: HashSet<u32> = HashSet::new();
        let mut visited_frame_nodes: HashSet<u32> = HashSet::new();
        let mut spatial_children: HashMap<u32, Vec<u32>> = HashMap::new();
        let mut frame_children: HashMap<u32, Vec<u32>> = HashMap::new();
        let mut frame_roots: Vec<u32> = Vec::new();

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
            let mut frame = run.context.frame;
            while !frame.is_none() && visited_frame_nodes.insert(frame.0) {
                let parent = self.frame_nodes[frame.0 as usize].parent;
                if parent.is_none() {
                    frame_roots.push(frame.0);
                } else {
                    frame_children.entry(parent.0).or_default().push(frame.0);
                }
                frame = parent;
            }
        }

        let mut text = String::from("AccumulatedVisualContext Tree:\n");
        let mut append_owner = |text: &mut String, is_frame: bool, index: u32| {
            if let Some(label) = owner_label(is_frame, index) {
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
            &mut |text: &mut String, index: u32| append_owner(text, false, index),
            VISUAL_VIEWPORT_NODE_INDEX.0,
            2,
        );
        if !frame_roots.is_empty() {
            text.push_str("  frames:\n");
            for root in frame_roots {
                dump_subtree(
                    &mut text,
                    &frame_children,
                    &|index: u32| {
                        format!(
                            "[f{index} in s{}] {}",
                            self.frame_nodes[index as usize].spatial.0,
                            self.dump_frame_node(FrameNodeIndex(index))
                        )
                    },
                    &mut |text: &mut String, index: u32| append_owner(text, true, index),
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
        AnchorScrollShift, BackfaceVisibilityData, ClipData, ClipMode, EffectsData, FrameData, FrameNodeIndex,
        MaskData, MaskLayerOrigin, PerspectiveData, ScrollData, SpatialData, StickyData, TransformData,
        TransformDataRole, VISUAL_VIEWPORT_NODE_INDEX, VisualContextTree, scroll_state::NO_SCROLL_STATE_SLOT,
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
    fn frame_nodes_dump_like_the_display_list_expectations() {
        let mut tree = tree();
        let plain_clip = tree.append_frame(
            FrameData::Clip(ClipData {
                rect: FloatRect::new(11.0, 10.0, 100.0, 16.0),
                corner_radii: CornerRadii::default(),
                mode: ClipMode::Intersect,
            }),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let rounded_difference_clip = tree.append_frame(
            FrameData::Clip(ClipData {
                rect: FloatRect::new(0.5, 0.0, 10.0, 10.0),
                corner_radii: CornerRadii::uniform(3),
                mode: ClipMode::Difference,
            }),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let plain_effects = tree.append_frame(
            FrameData::Effects(EffectsData {
                opacity: 1.0,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
            }),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let full_effects = tree.append_frame(
            FrameData::Effects(EffectsData {
                opacity: 0.5,
                blend_mode: CompositingAndBlendingOperator::Multiply,
                filter: Some(std::rc::Rc::new(vec![1, 2, 3])),
            }),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let mask = tree.append_frame(
            FrameData::Mask(MaskData {
                rect: IntRect::new(1, 2, 30, 40),
                kind: MaskKind::Luminance,
                origin: MaskLayerOrigin::SvgMask,
            }),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );

        assert_eq!(tree.dump_frame_node(plain_clip), "clip=[11,10 100x16]");
        assert_eq!(
            tree.dump_frame_node(rounded_difference_clip),
            "clip=[0.5,0 10x10] radii=(3,3,3,3) mode=difference"
        );
        assert_eq!(tree.dump_frame_node(plain_effects), "effects=[]");
        assert_eq!(
            tree.dump_frame_node(full_effects),
            format!(
                "effects=[opacity=0.5 blend_mode={} filter]",
                CompositingAndBlendingOperator::Multiply as i32
            )
        );
        assert_eq!(
            tree.dump_frame_node(mask),
            "mask=[1,2 30x40] kind=luminance origin=svg-mask"
        );
    }
}

#[cfg(test)]
mod section_dump_tests {
    use crate::layout::node_data::NodeSlotId;
    use crate::painting::display_list::commands::{
        ContextRef, DisplayListCommandRun, FrameNodeIndex, SpatialNodeIndex,
    };
    use crate::painting::visual_context::{
        ClipData, ClipMode, EffectsData, FrameData, ScrollData, SpatialData, TransformData, TransformDataRole,
        VISUAL_VIEWPORT_NODE_INDEX, VisualContextTree, scroll_state::NO_SCROLL_STATE_SLOT,
    };
    use libgfx_rust::{CompositingAndBlendingOperator, CornerRadii, FloatMatrix4x4, FloatPoint, FloatRect, IntRect};

    fn run(spatial: SpatialNodeIndex, frame: FrameNodeIndex) -> DisplayListCommandRun {
        DisplayListCommandRun {
            offset: 0,
            size: 0,
            context: ContextRef { spatial, frame },
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
        let clip_frame = tree.append_frame(
            FrameData::Clip(ClipData {
                rect: FloatRect::new(1.0, 2.0, 3.0, 4.0),
                corner_radii: CornerRadii::default(),
                mode: ClipMode::Intersect,
            }),
            FrameNodeIndex::NONE,
            scroll_node,
        );
        let effects_frame = tree.append_frame(
            FrameData::Effects(EffectsData {
                opacity: 0.5,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
            }),
            clip_frame,
            scroll_node,
        );
        let _ = unreachable_node;
        let runs = [
            run(scroll_node, effects_frame),
            run(VISUAL_VIEWPORT_NODE_INDEX, FrameNodeIndex::NONE),
        ];
        let text = tree.dump_nodes_reachable_from_runs(&runs, |is_frame, index| match (is_frame, index) {
            (false, 1) => Some("BlockContainer<DIV>#scroller".to_string()),
            (true, 0) => Some("BlockContainer<DIV>#scroller".to_string()),
            _ => None,
        });
        assert_eq!(
            text,
            "AccumulatedVisualContext Tree:\n  spatial:\n    [s0] transform=[1,0,0,1,0,0] origin=(0,0)\n      [s1] scroll (BlockContainer<DIV>#scroller)\n  frames:\n    [f0 in s1] clip=[1,2 3x4] (BlockContainer<DIV>#scroller)\n      [f1 in s1] effects=[opacity=0.5]\n"
        );
    }
}
