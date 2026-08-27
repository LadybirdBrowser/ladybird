/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub mod basic_shapes;
pub mod build;
pub mod local_frames;
pub mod nested;
pub mod node_values;
pub mod refresh;
pub mod scroll_state;

use crate::painting::display_list::commands::OptionalF32;
use crate::painting::paintable_data::BorderEdge;
use libgfx_rust::{
    CompositingAndBlendingOperator, CornerRadii, FloatMatrix4x4, FloatPoint, FloatRect, FloatSize, IntRect, MaskKind,
    WindingRule,
};
use scroll_state::{NO_SCROLL_STATE_SLOT, ScrollStateSlot};
use std::ffi::c_void;

pub use crate::painting::display_list::commands::{
    ContextRef, FrameNodeIndex, SpatialNodeIndex, VISUAL_VIEWPORT_NODE_INDEX,
};

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
    pub sorting_context_root_index: Option<SpatialNodeIndex>,
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
    pub plane_root_index: SpatialNodeIndex,
    pub flattens_inherited_transform: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum ClipMode {
    Intersect,
    Difference,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ClipData {
    pub rect: FloatRect,
    pub corner_radii: CornerRadii,
    pub mode: ClipMode,
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
    pub filter: Option<EffectsFilter>,
}

#[derive(Clone)]
pub enum EffectsFilter {
    Bytes(std::rc::Rc<Vec<u8>>),
    Host(std::rc::Rc<FilterHandle>),
}

impl FrameData {
    pub fn layer_blending_with(blend_mode: CompositingAndBlendingOperator) -> Self {
        FrameData::Effects(EffectsData {
            opacity: 1.0,
            blend_mode,
            filter: None,
        })
    }

    pub fn rect_clip(rect: FloatRect) -> Self {
        FrameData::Clip(ClipData {
            rect,
            corner_radii: CornerRadii::default(),
            mode: ClipMode::Intersect,
        })
    }
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
    pub state_slot: ScrollStateSlot,
}

// A sticky box's shift is derived when the scroll state snapshot is resolved, from the scroller's
// entry and the parent sticky chain. Both references follow the containing block chain, which
// continues through fixed-position ancestors, so they need not be spatial ancestors of the node.
// Geometry is in device pixels.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct StickyData {
    pub scroller: SpatialNodeIndex,
    pub parent_sticky: Option<SpatialNodeIndex>,
    pub position_relative_to_scroller: FloatPoint,
    pub border_box_size: FloatSize,
    pub scrollport_size: FloatSize,
    pub containing_block_region: FloatRect,
    pub needs_parent_offset_adjustment: bool,
    pub inset_top: Option<f32>,
    pub inset_right: Option<f32>,
    pub inset_bottom: Option<f32>,
    pub inset_left: Option<f32>,
    pub state_slot: ScrollStateSlot,
}

impl StickyData {
    pub fn unconstrained(
        scroller: SpatialNodeIndex,
        parent_sticky: Option<SpatialNodeIndex>,
        state_slot: ScrollStateSlot,
    ) -> Self {
        Self {
            scroller,
            parent_sticky,
            position_relative_to_scroller: FloatPoint::default(),
            border_box_size: FloatSize::default(),
            scrollport_size: FloatSize::default(),
            containing_block_region: FloatRect::default(),
            needs_parent_offset_adjustment: false,
            inset_top: None,
            inset_right: None,
            inset_bottom: None,
            inset_left: None,
            state_slot,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AnchorScrollShift {
    pub scroll_node_index: SpatialNodeIndex,
    pub negate: bool,
    pub compensate_horizontal_scroll: bool,
    pub compensate_vertical_scroll: bool,
}

pub enum SpatialData {
    Scroll(ScrollData),
    Sticky(StickyData),
    Transform(TransformData),
    Perspective(PerspectiveData),
    BackfaceVisibility(BackfaceVisibilityData),
    AnchorScrollShift(AnchorScrollShift),
}

pub enum FrameData {
    Clip(ClipData),
    ClipPath(ClipPathData),
    Effects(EffectsData),
    Mask(MaskData),
}

impl SpatialData {
    fn kind(&self) -> crate::painting::host::FfiVisualContextNodeKind {
        use crate::painting::host::FfiVisualContextNodeKind;
        match self {
            Self::Scroll(_) => FfiVisualContextNodeKind::Scroll,
            Self::Sticky(_) => FfiVisualContextNodeKind::Sticky,
            Self::Transform(_) => FfiVisualContextNodeKind::Transform,
            Self::Perspective(_) => FfiVisualContextNodeKind::Perspective,
            Self::BackfaceVisibility(_) => FfiVisualContextNodeKind::BackfaceVisibility,
            Self::AnchorScrollShift(_) => FfiVisualContextNodeKind::AnchorScrollShift,
        }
    }

    pub fn is_scroll_like(&self) -> bool {
        matches!(self, Self::Scroll(_) | Self::Sticky(_))
    }
}

impl FrameData {
    fn kind(&self) -> crate::painting::host::FfiVisualContextNodeKind {
        use crate::painting::host::FfiVisualContextNodeKind;
        match self {
            Self::Clip(_) => FfiVisualContextNodeKind::Clip,
            Self::ClipPath(_) => FfiVisualContextNodeKind::ClipPath,
            Self::Effects(_) => FfiVisualContextNodeKind::Effects,
            Self::Mask(_) => FfiVisualContextNodeKind::Mask,
        }
    }

    fn is_empty_clip(&self) -> bool {
        match self {
            Self::Clip(clip) => clip.mode == ClipMode::Intersect && clip.rect.is_empty(),
            Self::ClipPath(clip_path) => clip_path.path_bounds_are_empty,
            Self::Mask(mask) => mask.rect.is_empty(),
            Self::Effects(_) => false,
        }
    }
}

pub struct SpatialNode {
    pub data: SpatialData,
    pub parent: SpatialNodeIndex,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum PieceKey {
    Box,
    Piece(u16),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum FrameRole {
    Structural,
    RootIsolation,
    ContentCornerClip,
    ContentClip,
    OuterShadowClip {
        piece: PieceKey,
    },
    FieldsetBackgroundClip,
    FieldsetTopBorderBand,
    LegendCutout,
    BackgroundIsolation {
        piece: PieceKey,
    },
    BackgroundLayerCornerClip {
        piece: PieceKey,
        layer: u16,
        isolated: bool,
    },
    BackgroundLayerClip {
        piece: PieceKey,
        layer: u16,
        isolated: bool,
    },
    BackgroundLayerBlend {
        piece: PieceKey,
        layer: u16,
        isolated: bool,
    },
    // https://drafts.csswg.org/css-backgrounds-4/#valdef-background-clip-text
    BackgroundTextClip {
        piece: PieceKey,
    },
    BackgroundTextContentLayer {
        piece: PieceKey,
    },
    BackgroundTextMask {
        piece: PieceKey,
    },
    PatternedEdge {
        owner: PatternedEdgeOwner,
        piece: PieceKey,
        edge: BorderEdge,
    },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum PatternedEdgeOwner {
    Border,
    Outline,
}

pub struct FrameNode {
    pub data: FrameData,
    pub parent: FrameNodeIndex,
    pub spatial: SpatialNodeIndex,
    pub has_empty_effective_clip: bool,
    pub role: FrameRole,
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
    pub spatial_nodes: Vec<SpatialNode>,
    pub frame_nodes: Vec<FrameNode>,
    pub root_is_visual_viewport: bool,
    pub root_isolation_frame: Option<FrameNodeIndex>,
    pub version: u64,
    pub reused_previous_version: bool,
}

impl VisualContextTree {
    pub fn create(visual_viewport_transform: TransformData) -> Self {
        Self::with_root(visual_viewport_transform, true)
    }

    pub fn create_with_content_root(content_transform: TransformData) -> Self {
        Self::with_root(content_transform, false)
    }

    fn with_root(root_transform: TransformData, root_is_visual_viewport: bool) -> Self {
        Self {
            spatial_nodes: vec![SpatialNode {
                data: SpatialData::Transform(root_transform),
                parent: VISUAL_VIEWPORT_NODE_INDEX,
            }],
            frame_nodes: Vec::new(),
            root_is_visual_viewport,
            root_isolation_frame: None,
            version: 0,
            reused_previous_version: false,
        }
    }

    pub fn append_spatial(&mut self, data: SpatialData, parent: SpatialNodeIndex) -> SpatialNodeIndex {
        assert!((parent.0 as usize) < self.spatial_nodes.len());
        self.spatial_nodes.push(SpatialNode { data, parent });
        SpatialNodeIndex((self.spatial_nodes.len() - 1) as u32)
    }

    pub fn append_spatial_under(&mut self, context: ContextRef, data: SpatialData) -> ContextRef {
        ContextRef {
            spatial: self.append_spatial(data, context.spatial),
            ..context
        }
    }

    pub fn append_frame_under(&mut self, context: ContextRef, data: FrameData) -> ContextRef {
        ContextRef {
            frame: self.append_frame(data, context.frame, context.spatial),
            ..context
        }
    }

    pub fn append_frame(
        &mut self,
        data: FrameData,
        parent: FrameNodeIndex,
        spatial: SpatialNodeIndex,
    ) -> FrameNodeIndex {
        self.append_frame_with_role(data, parent, spatial, FrameRole::Structural)
    }

    pub fn append_frame_with_role(
        &mut self,
        data: FrameData,
        parent: FrameNodeIndex,
        spatial: SpatialNodeIndex,
        role: FrameRole,
    ) -> FrameNodeIndex {
        assert!((spatial.0 as usize) < self.spatial_nodes.len());
        let inherited_empty_clip = if parent.is_none() {
            false
        } else {
            let parent_node = &self.frame_nodes[parent.0 as usize];
            debug_assert!(self.spatial_is_ancestor_or_self(parent_node.spatial, spatial));
            parent_node.has_empty_effective_clip
        };
        self.frame_nodes.push(FrameNode {
            has_empty_effective_clip: inherited_empty_clip || data.is_empty_clip(),
            data,
            parent,
            spatial,
            role,
        });
        FrameNodeIndex((self.frame_nodes.len() - 1) as u32)
    }

    fn spatial_is_ancestor_or_self(&self, ancestor: SpatialNodeIndex, mut node: SpatialNodeIndex) -> bool {
        loop {
            if node == ancestor {
                return true;
            }
            if node == VISUAL_VIEWPORT_NODE_INDEX {
                return false;
            }
            node = self.spatial_nodes[node.0 as usize].parent;
        }
    }

    pub fn set_visual_viewport_transform(&mut self, transform: TransformData) {
        assert!(matches!(
            self.spatial_nodes[VISUAL_VIEWPORT_NODE_INDEX.0 as usize].data,
            SpatialData::Transform(_)
        ));
        self.spatial_nodes[VISUAL_VIEWPORT_NODE_INDEX.0 as usize].data = SpatialData::Transform(transform);
    }

    pub fn is_compatible_with(&self, other: &Self) -> bool {
        if self.spatial_nodes.len() != other.spatial_nodes.len() || self.frame_nodes.len() != other.frame_nodes.len() {
            return false;
        }
        if self.root_isolation_frame != other.root_isolation_frame {
            return false;
        }
        let spatial_compatible = self
            .spatial_nodes
            .iter()
            .zip(&other.spatial_nodes)
            .all(|(node, other_node)| node.parent == other_node.parent && node.data.kind() == other_node.data.kind());
        spatial_compatible
            && self
                .frame_nodes
                .iter()
                .zip(&other.frame_nodes)
                .all(|(node, other_node)| {
                    node.parent == other_node.parent
                        && node.spatial == other_node.spatial
                        && node.has_empty_effective_clip == other_node.has_empty_effective_clip
                        && node.data.kind() == other_node.data.kind()
                })
    }

    pub fn scroll_state_slot_for_node(&self, index: SpatialNodeIndex) -> ScrollStateSlot {
        if index == VISUAL_VIEWPORT_NODE_INDEX {
            return NO_SCROLL_STATE_SLOT;
        }
        match &self.spatial_nodes[index.0 as usize].data {
            SpatialData::Scroll(scroll) => scroll.state_slot,
            SpatialData::Sticky(sticky) => sticky.state_slot,
            _ => panic!("spatial node {} is not a scroll-like node", index.0),
        }
    }

    pub fn empty_effective_clips_by_frame(&self) -> Vec<bool> {
        self.frame_nodes
            .iter()
            .map(|node| node.has_empty_effective_clip)
            .collect()
    }
}

fn empty_export(
    kind: crate::painting::host::FfiVisualContextNodeKind,
) -> crate::painting::host::FfiVisualContextNodeExport {
    crate::painting::host::FfiVisualContextNodeExport {
        kind,
        parent: 0,
        spatial: 0,
        matrix: FloatMatrix4x4::default(),
        origin: FloatPoint::default(),
        flattens_inherited_transform: false,
        transform_role: TransformDataRole::CssTransform,
        has_sorting_context_root: false,
        synthetic_plane: false,
        rect: IntRect::default(),
        corner_radii: CornerRadii::default(),
        clip_rect: FloatRect::default(),
        clip_mode: ClipMode::Intersect,
        opacity: 1.0,
        blend_mode: CompositingAndBlendingOperator::Normal,
        filter: std::ptr::null_mut(),
        filter_bytes: std::ptr::null(),
        filter_bytes_length: 0,
        path: std::ptr::null_mut(),
        winding_rule: WindingRule::Nonzero,
        mask_kind: MaskKind::Alpha,
        mask_origin: MaskLayerOrigin::CssMaskLayers,
        index_value: 0,
        sticky_parent_sticky_index: 0,
        sticky_position_relative_to_scroller: FloatPoint::default(),
        sticky_border_box_size: FloatSize::default(),
        sticky_scrollport_size: FloatSize::default(),
        sticky_containing_block_region: FloatRect::default(),
        sticky_needs_parent_offset_adjustment: false,
        sticky_inset_top: OptionalF32::none(),
        sticky_inset_right: OptionalF32::none(),
        sticky_inset_bottom: OptionalF32::none(),
        sticky_inset_left: OptionalF32::none(),
        negate: false,
        compensate_horizontal_scroll: false,
        compensate_vertical_scroll: false,
    }
}

pub fn export_spatial_node(node: &SpatialNode) -> crate::painting::host::FfiVisualContextNodeExport {
    let mut out = empty_export(node.data.kind());
    out.parent = node.parent.0;
    match &node.data {
        SpatialData::Scroll(_) => {}
        SpatialData::Sticky(sticky) => {
            out.index_value = sticky.scroller.0;
            out.sticky_parent_sticky_index = sticky.parent_sticky.map_or(0, |index| index.0);
            out.sticky_position_relative_to_scroller = sticky.position_relative_to_scroller;
            out.sticky_border_box_size = sticky.border_box_size;
            out.sticky_scrollport_size = sticky.scrollport_size;
            out.sticky_containing_block_region = sticky.containing_block_region;
            out.sticky_needs_parent_offset_adjustment = sticky.needs_parent_offset_adjustment;
            out.sticky_inset_top = sticky.inset_top.into();
            out.sticky_inset_right = sticky.inset_right.into();
            out.sticky_inset_bottom = sticky.inset_bottom.into();
            out.sticky_inset_left = sticky.inset_left.into();
        }
        SpatialData::Transform(transform) => {
            out.matrix = transform.matrix;
            out.origin = transform.origin;
            out.flattens_inherited_transform = transform.flattens_inherited_transform;
            out.transform_role = transform.role;
            if let Some(root) = transform.sorting_context_root_index {
                out.has_sorting_context_root = true;
                out.index_value = root.0;
            }
            out.synthetic_plane = transform.synthetic_plane;
        }
        SpatialData::Perspective(perspective) => {
            out.matrix = perspective.matrix;
            out.flattens_inherited_transform = perspective.flattens_inherited_transform;
        }
        SpatialData::BackfaceVisibility(backface) => {
            out.index_value = backface.plane_root_index.0;
            out.flattens_inherited_transform = backface.flattens_inherited_transform;
        }
        SpatialData::AnchorScrollShift(shift) => {
            out.index_value = shift.scroll_node_index.0;
            out.negate = shift.negate;
            out.compensate_horizontal_scroll = shift.compensate_horizontal_scroll;
            out.compensate_vertical_scroll = shift.compensate_vertical_scroll;
        }
    }
    out
}

pub fn export_frame_node(node: &FrameNode) -> crate::painting::host::FfiVisualContextNodeExport {
    let mut out = empty_export(node.data.kind());
    out.parent = node.parent.0;
    out.spatial = node.spatial.0;
    match &node.data {
        FrameData::Clip(clip) => {
            out.clip_rect = clip.rect;
            out.corner_radii = clip.corner_radii;
            out.clip_mode = clip.mode;
        }
        FrameData::ClipPath(clip_path) => {
            out.path = clip_path.path.as_raw();
            out.rect = clip_path.bounding_rect;
            out.winding_rule = clip_path.fill_rule;
        }
        FrameData::Effects(effects) => {
            out.opacity = effects.opacity;
            out.blend_mode = effects.blend_mode;
            if let Some(filter) = &effects.filter {
                match filter {
                    EffectsFilter::Bytes(bytes) => {
                        out.filter_bytes = bytes.as_ptr();
                        out.filter_bytes_length = bytes.len();
                    }
                    EffectsFilter::Host(filter) => out.filter = filter.as_raw(),
                }
            }
        }
        FrameData::Mask(mask) => {
            out.rect = mask.rect;
            out.mask_kind = mask.kind;
            out.mask_origin = mask.origin;
        }
    }
    out
}
