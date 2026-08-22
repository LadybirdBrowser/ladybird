/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub mod basic_shapes;
pub mod build;
pub mod nested;
pub mod node_values;
pub mod refresh;
pub mod scroll_state;

use libgfx_rust::{
    CompositingAndBlendingOperator, CornerRadii, FloatMatrix4x4, FloatPoint, IntRect, MaskKind, WindingRule,
};
use scroll_state::{NO_SCROLL_STATE_SLOT, ScrollStateSlot};
use std::ffi::c_void;

pub const VISUAL_VIEWPORT_NODE_INDEX: usize = 0;

pub struct FilterHandle {
    raw: *mut c_void,
}

unsafe extern "C" {
    fn ladybird_gfx_filter_destroy(filter: *mut c_void);
}

impl FilterHandle {
    /// # Safety
    ///
    /// `raw` must be a heap-allocated `Gfx::Filter` owned by nobody else.
    pub unsafe fn adopt(raw: *mut c_void) -> Self {
        Self { raw }
    }
    pub fn as_raw(&self) -> *mut c_void {
        self.raw
    }
}

impl Drop for FilterHandle {
    fn drop(&mut self) {
        // SAFETY: adopt() took sole ownership of the filter.
        unsafe { ladybird_gfx_filter_destroy(self.raw) };
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum TransformDataRole {
    CssTransform,
    SvgViewportTransform,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct TransformData {
    pub matrix: FloatMatrix4x4,
    pub origin: FloatPoint,
    pub sorting_context_root_index: Option<usize>,
    pub flattens_inherited_transform: bool,
    pub role: TransformDataRole,
    pub synthetic_plane: bool,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PerspectiveData {
    pub matrix: FloatMatrix4x4,
    pub flattens_inherited_transform: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BackfaceVisibilityData {
    pub plane_root_index: usize,
    pub flattens_inherited_transform: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ClipData {
    pub rect: IntRect,
    pub corner_radii: CornerRadii,
}

pub struct ClipPathData {
    pub path: std::rc::Rc<libgfx_rust::path::OwnedPath>,
    pub bounding_rect: IntRect,
    pub fill_rule: WindingRule,
    pub path_bounds_are_empty: bool,
}

pub struct EffectsData {
    pub opacity: f32,
    pub blend_mode: CompositingAndBlendingOperator,
    pub filter: Option<std::rc::Rc<FilterHandle>>,
}

impl EffectsData {
    pub fn needs_layer(&self) -> bool {
        self.opacity < 1.0 || self.blend_mode != CompositingAndBlendingOperator::Normal || self.filter.is_some()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum MaskLayerOrigin {
    CssMaskLayers,
    SvgMask,
    SvgClip,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MaskData {
    pub rect: IntRect,
    pub kind: MaskKind,
    pub origin: MaskLayerOrigin,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ScrollData {
    pub is_sticky: bool,
    pub state_slot: ScrollStateSlot,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ScrollCompensation {
    pub scroll_node_index: usize,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AnchorScrollShift {
    pub scroll_node_index: usize,
    pub negate: bool,
    pub compensate_horizontal_scroll: bool,
    pub compensate_vertical_scroll: bool,
}

pub enum VisualContextData {
    Scroll(ScrollData),
    Clip(ClipData),
    Transform(TransformData),
    Perspective(PerspectiveData),
    BackfaceVisibility(BackfaceVisibilityData),
    ClipPath(ClipPathData),
    Effects(EffectsData),
    ScrollCompensation(ScrollCompensation),
    AnchorScrollShift(AnchorScrollShift),
    Mask(MaskData),
}

impl VisualContextData {
    fn kind(&self) -> crate::painting::host::FfiVisualContextNodeKind {
        use crate::painting::host::FfiVisualContextNodeKind;
        match self {
            Self::Scroll(_) => FfiVisualContextNodeKind::Scroll,
            Self::Clip(_) => FfiVisualContextNodeKind::Clip,
            Self::Transform(_) => FfiVisualContextNodeKind::Transform,
            Self::Perspective(_) => FfiVisualContextNodeKind::Perspective,
            Self::BackfaceVisibility(_) => FfiVisualContextNodeKind::BackfaceVisibility,
            Self::ClipPath(_) => FfiVisualContextNodeKind::ClipPath,
            Self::Effects(_) => FfiVisualContextNodeKind::Effects,
            Self::ScrollCompensation(_) => FfiVisualContextNodeKind::ScrollCompensation,
            Self::AnchorScrollShift(_) => FfiVisualContextNodeKind::AnchorScrollShift,
            Self::Mask(_) => FfiVisualContextNodeKind::Mask,
        }
    }
}

pub struct VisualContextNode {
    pub data: VisualContextData,
    pub parent_index: usize,
    pub depth: usize,
    pub has_empty_effective_clip: bool,
}

#[derive(Default)]
pub struct VisualContextState {
    pub tree: Option<VisualContextTree>,
    pub paintables_with_mask_nodes: Vec<crate::layout::node_data::NodeSlotId>,
    pub scroll_state: scroll_state::ScrollState,
    pub scroll_state_snapshot: Vec<FloatPoint>,
    pub needs_to_refresh_scroll_state: bool,
    pub build_count: u64,
    pub next_tree_version: u64,
}

impl VisualContextState {
    pub fn allocate_tree_version(&mut self) -> u64 {
        self.next_tree_version += 1;
        self.next_tree_version
    }

    pub fn tree_version(&self) -> u64 {
        self.tree.as_ref().map_or(0, |tree| tree.version)
    }
}

pub struct VisualContextTree {
    pub nodes: Vec<VisualContextNode>,
    pub root_is_visual_viewport: bool,
    pub version: u64,
    pub reused_previous_version: bool,
}

impl VisualContextTree {
    pub fn create(visual_viewport_transform: TransformData) -> Self {
        Self {
            nodes: vec![VisualContextNode {
                data: VisualContextData::Transform(visual_viewport_transform),
                parent_index: 0,
                depth: 0,
                has_empty_effective_clip: false,
            }],
            root_is_visual_viewport: true,
            version: 0,
            reused_previous_version: false,
        }
    }

    pub fn create_with_content_root(content_transform: TransformData) -> Self {
        Self {
            nodes: vec![VisualContextNode {
                data: VisualContextData::Transform(content_transform),
                parent_index: 0,
                depth: 0,
                has_empty_effective_clip: false,
            }],
            root_is_visual_viewport: false,
            version: 0,
            reused_previous_version: false,
        }
    }

    pub fn append(&mut self, data: VisualContextData, parent_index: usize) -> usize {
        assert!(parent_index < self.nodes.len());
        let depth = self.nodes[parent_index].depth + 1;
        let empty_clip = if self.nodes[parent_index].has_empty_effective_clip {
            true
        } else {
            match &data {
                VisualContextData::Clip(clip) => clip.rect.is_empty(),
                VisualContextData::ClipPath(clip_path) => clip_path.path_bounds_are_empty,
                VisualContextData::Mask(mask) => mask.rect.is_empty(),
                _ => false,
            }
        };
        self.nodes.push(VisualContextNode {
            data,
            parent_index,
            depth,
            has_empty_effective_clip: empty_clip,
        });
        self.nodes.len() - 1
    }

    pub fn set_visual_viewport_transform(&mut self, transform: TransformData) {
        assert!(matches!(
            self.nodes[VISUAL_VIEWPORT_NODE_INDEX].data,
            VisualContextData::Transform(_)
        ));
        self.nodes[VISUAL_VIEWPORT_NODE_INDEX].data = VisualContextData::Transform(transform);
    }

    pub fn is_compatible_with(&self, other: &Self) -> bool {
        if self.nodes.len() != other.nodes.len() {
            return false;
        }
        self.nodes.iter().zip(&other.nodes).all(|(node, other_node)| {
            node.parent_index == other_node.parent_index
                && node.has_empty_effective_clip == other_node.has_empty_effective_clip
                && node.data.kind() == other_node.data.kind()
        })
    }

    pub fn scroll_state_slot_for_node(&self, index: usize) -> ScrollStateSlot {
        if index == 0 {
            return NO_SCROLL_STATE_SLOT;
        }
        match &self.nodes[index].data {
            VisualContextData::Scroll(scroll) => scroll.state_slot,
            _ => panic!("visual context node {index} is not a scroll node"),
        }
    }
}

pub fn export_node(node: &VisualContextNode) -> crate::painting::host::FfiVisualContextNodeExport {
    use crate::painting::host::FfiVisualContextNodeExport;
    let mut out = FfiVisualContextNodeExport {
        kind: node.data.kind(),
        parent_index: node.parent_index,
        matrix: [0.0; 16],
        origin: [0.0; 2],
        flattens_inherited_transform: false,
        transform_role: 0,
        has_sorting_context_root: false,
        synthetic_plane: false,
        rect: [0; 4],
        corner_radii: [0; 8],
        opacity: 1.0,
        blend_mode: 0,
        filter: std::ptr::null_mut(),
        path: std::ptr::null_mut(),
        winding_rule: 0,
        mask_kind: 0,
        mask_origin: 0,
        index_value: 0,
        is_sticky: false,
        state_slot: 0,
        negate: false,
        compensate_horizontal_scroll: false,
        compensate_vertical_scroll: false,
    };
    let write_matrix = |matrix: &FloatMatrix4x4, out: &mut [f32; 16]| {
        for (row, values) in matrix.elements.iter().enumerate() {
            for (column, value) in values.iter().enumerate() {
                out[row * 4 + column] = *value;
            }
        }
    };
    let write_rect = |rect: &IntRect, out: &mut [i32; 4]| {
        *out = [rect.x, rect.y, rect.width, rect.height];
    };
    match &node.data {
        VisualContextData::Scroll(scroll) => {
            out.is_sticky = scroll.is_sticky;
            out.state_slot = scroll.state_slot;
        }
        VisualContextData::Clip(clip) => {
            write_rect(&clip.rect, &mut out.rect);
            let radii = &clip.corner_radii;
            out.corner_radii = [
                radii.top_left.horizontal_radius,
                radii.top_left.vertical_radius,
                radii.top_right.horizontal_radius,
                radii.top_right.vertical_radius,
                radii.bottom_right.horizontal_radius,
                radii.bottom_right.vertical_radius,
                radii.bottom_left.horizontal_radius,
                radii.bottom_left.vertical_radius,
            ];
        }
        VisualContextData::Transform(transform) => {
            write_matrix(&transform.matrix, &mut out.matrix);
            out.origin = [transform.origin.x, transform.origin.y];
            out.flattens_inherited_transform = transform.flattens_inherited_transform;
            out.transform_role = transform.role as u8;
            if let Some(root) = transform.sorting_context_root_index {
                out.has_sorting_context_root = true;
                out.index_value = root;
            }
            out.synthetic_plane = transform.synthetic_plane;
        }
        VisualContextData::Perspective(perspective) => {
            write_matrix(&perspective.matrix, &mut out.matrix);
            out.flattens_inherited_transform = perspective.flattens_inherited_transform;
        }
        VisualContextData::BackfaceVisibility(backface) => {
            out.index_value = backface.plane_root_index;
            out.flattens_inherited_transform = backface.flattens_inherited_transform;
        }
        VisualContextData::ClipPath(clip_path) => {
            out.path = clip_path.path.as_raw();
            write_rect(&clip_path.bounding_rect, &mut out.rect);
            out.winding_rule = clip_path.fill_rule as i32;
        }
        VisualContextData::Effects(effects) => {
            out.opacity = effects.opacity;
            out.blend_mode = effects.blend_mode as i32;
            out.filter = effects
                .filter
                .as_ref()
                .map_or(std::ptr::null_mut(), |filter| filter.as_raw());
        }
        VisualContextData::ScrollCompensation(compensation) => {
            out.index_value = compensation.scroll_node_index;
        }
        VisualContextData::AnchorScrollShift(shift) => {
            out.index_value = shift.scroll_node_index;
            out.negate = shift.negate;
            out.compensate_horizontal_scroll = shift.compensate_horizontal_scroll;
            out.compensate_vertical_scroll = shift.compensate_vertical_scroll;
        }
        VisualContextData::Mask(mask) => {
            write_rect(&mask.rect, &mut out.rect);
            out.mask_kind = mask.kind as i32;
            out.mask_origin = mask.origin as u8;
        }
    }
    out
}
