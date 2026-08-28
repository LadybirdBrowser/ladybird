/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum SpatialNodeShape {
    Scroll {
        parent: SpatialNodeIndex,
        registry_parent_node: SpatialNodeIndex,
    },
    Sticky {
        parent: SpatialNodeIndex,
        registry_parent_node: SpatialNodeIndex,
    },
    Transform {
        parent: SpatialNodeIndex,
        sorting_context_root_index: Option<SpatialNodeIndex>,
        flattens_inherited_transform: bool,
        role: TransformDataRole,
        synthetic_plane: bool,
    },
    Perspective {
        parent: SpatialNodeIndex,
        flattens_inherited_transform: bool,
    },
    BackfaceVisibility {
        parent: SpatialNodeIndex,
        plane_root_index: SpatialNodeIndex,
        flattens_inherited_transform: bool,
    },
    AnchorScrollShift {
        parent: SpatialNodeIndex,
        scroll_node_index: SpatialNodeIndex,
        negate: bool,
        compensate_horizontal_scroll: bool,
        compensate_vertical_scroll: bool,
    },
    Dead,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum FrameShapeKind {
    Clip { mode: ClipMode },
    ClipPath,
    Effects,
    Mask { origin: MaskLayerOrigin },
    Dead,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct FrameNodeShape {
    pub kind: FrameShapeKind,
    pub parent: FrameNodeIndex,
    pub spatial: SpatialNodeIndex,
    pub role: FrameRole,
}

pub(crate) fn spatial_node_shape(node: &SpatialNode) -> SpatialNodeShape {
    let parent = node.parent;
    match &node.data {
        SpatialData::Scroll(scroll) => SpatialNodeShape::Scroll {
            parent,
            registry_parent_node: scroll.registry_parent_node,
        },
        SpatialData::Sticky(sticky) => SpatialNodeShape::Sticky {
            parent,
            registry_parent_node: sticky.registry_parent_node,
        },
        SpatialData::Transform(transform) => SpatialNodeShape::Transform {
            parent,
            sorting_context_root_index: transform.sorting_context_root_index,
            flattens_inherited_transform: transform.flattens_inherited_transform,
            role: transform.role,
            synthetic_plane: transform.synthetic_plane,
        },
        SpatialData::Perspective(perspective) => SpatialNodeShape::Perspective {
            parent,
            flattens_inherited_transform: perspective.flattens_inherited_transform,
        },
        SpatialData::BackfaceVisibility(backface) => SpatialNodeShape::BackfaceVisibility {
            parent,
            plane_root_index: backface.plane_root_index,
            flattens_inherited_transform: backface.flattens_inherited_transform,
        },
        SpatialData::AnchorScrollShift(shift) => SpatialNodeShape::AnchorScrollShift {
            parent,
            scroll_node_index: shift.scroll_node_index,
            negate: shift.negate,
            compensate_horizontal_scroll: shift.compensate_horizontal_scroll,
            compensate_vertical_scroll: shift.compensate_vertical_scroll,
        },
        SpatialData::Dead => SpatialNodeShape::Dead,
    }
}

pub(crate) fn frame_node_shape(node: &FrameNode) -> FrameNodeShape {
    let kind = match &node.data {
        FrameData::Clip(clip) => FrameShapeKind::Clip { mode: clip.mode },
        FrameData::ClipPath(_) => FrameShapeKind::ClipPath,
        FrameData::Effects(_) => FrameShapeKind::Effects,
        FrameData::Mask(mask) => FrameShapeKind::Mask { origin: mask.origin },
        FrameData::Dead => FrameShapeKind::Dead,
    };
    FrameNodeShape {
        kind,
        parent: node.parent,
        spatial: node.spatial,
        role: node.role,
    }
}

fn effects_filters_are_equal(a: Option<&std::rc::Rc<Vec<u8>>>, b: Option<&std::rc::Rc<Vec<u8>>>) -> bool {
    match (a, b) {
        (None, None) => true,
        (Some(a), Some(b)) => std::rc::Rc::ptr_eq(a, b) || a == b,
        _ => false,
    }
}

pub(crate) fn spatial_payloads_are_equal(a: &SpatialData, b: &SpatialData) -> bool {
    match (a, b) {
        (SpatialData::Scroll(a), SpatialData::Scroll(b)) => a == b,
        (SpatialData::Sticky(a), SpatialData::Sticky(b)) => a == b,
        (SpatialData::Transform(a), SpatialData::Transform(b)) => a == b,
        (SpatialData::Perspective(a), SpatialData::Perspective(b)) => a == b,
        (SpatialData::BackfaceVisibility(a), SpatialData::BackfaceVisibility(b)) => a == b,
        (SpatialData::AnchorScrollShift(a), SpatialData::AnchorScrollShift(b)) => a == b,
        (SpatialData::Dead, SpatialData::Dead) => true,
        _ => false,
    }
}

pub(crate) fn frame_payloads_are_equal(a: &FrameData, b: &FrameData) -> bool {
    match (a, b) {
        (FrameData::Clip(a), FrameData::Clip(b)) => a == b,
        (FrameData::ClipPath(a), FrameData::ClipPath(b)) => {
            std::rc::Rc::ptr_eq(&a.path, &b.path) && a.bounding_rect == b.bounding_rect && a.fill_rule == b.fill_rule
        }
        (FrameData::Effects(a), FrameData::Effects(b)) => {
            a.opacity == b.opacity
                && a.blend_mode == b.blend_mode
                && effects_filters_are_equal(a.filter.as_ref(), b.filter.as_ref())
        }
        (FrameData::Mask(a), FrameData::Mask(b)) => a == b,
        (FrameData::Dead, FrameData::Dead) => true,
        _ => false,
    }
}
