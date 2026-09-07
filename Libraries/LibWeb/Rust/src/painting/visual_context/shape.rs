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
pub(crate) enum ClipShapeKind {
    Rect { mode: ClipMode },
    Path,
    Dead,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ClipNodeShape {
    pub kind: ClipShapeKind,
    pub parent: ClipNodeIndex,
    pub spatial: SpatialNodeIndex,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum EffectShapeKind {
    Effects,
    Mask { origin: MaskLayerOrigin },
    BackgroundColorAnimation,
    Dead,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct EffectNodeShape {
    pub kind: EffectShapeKind,
    pub parent: EffectNodeIndex,
    pub spatial: SpatialNodeIndex,
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

pub(crate) fn clip_node_shape(node: &ClipNode) -> ClipNodeShape {
    let kind = match &node.data {
        ClipNodeData::Rect(clip) => ClipShapeKind::Rect { mode: clip.mode },
        ClipNodeData::Path(_) => ClipShapeKind::Path,
        ClipNodeData::Dead => ClipShapeKind::Dead,
    };
    ClipNodeShape {
        kind,
        parent: node.parent,
        spatial: node.spatial,
    }
}

pub(crate) fn effect_node_shape(node: &EffectNode) -> EffectNodeShape {
    let kind = match &node.data {
        EffectNodeData::Effects(_) => EffectShapeKind::Effects,
        EffectNodeData::Mask(mask) => EffectShapeKind::Mask { origin: mask.origin },
        EffectNodeData::BackgroundColorAnimation => EffectShapeKind::BackgroundColorAnimation,
        EffectNodeData::Dead => EffectShapeKind::Dead,
    };
    EffectNodeShape {
        kind,
        parent: node.parent,
        spatial: node.spatial,
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

pub(crate) fn clip_payloads_are_equal(a: &ClipNodeData, b: &ClipNodeData) -> bool {
    match (a, b) {
        (ClipNodeData::Rect(a), ClipNodeData::Rect(b)) => a == b,
        (ClipNodeData::Path(a), ClipNodeData::Path(b)) => {
            std::rc::Rc::ptr_eq(&a.path, &b.path) && a.bounding_rect == b.bounding_rect && a.fill_rule == b.fill_rule
        }
        (ClipNodeData::Dead, ClipNodeData::Dead) => true,
        _ => false,
    }
}

pub(crate) fn effect_payloads_are_equal(a: &EffectNodeData, b: &EffectNodeData) -> bool {
    match (a, b) {
        (EffectNodeData::Effects(a), EffectNodeData::Effects(b)) => {
            a.opacity == b.opacity
                && a.blend_mode == b.blend_mode
                && effects_filters_are_equal(a.filter.as_ref(), b.filter.as_ref())
        }
        (EffectNodeData::Mask(a), EffectNodeData::Mask(b)) => a == b,
        (EffectNodeData::BackgroundColorAnimation, EffectNodeData::BackgroundColorAnimation) => true,
        (EffectNodeData::Dead, EffectNodeData::Dead) => true,
        _ => false,
    }
}
