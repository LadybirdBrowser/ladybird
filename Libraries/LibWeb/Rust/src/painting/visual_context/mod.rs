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

use std::collections::HashMap;

use crate::painting::display_list::commands::OptionalF32;
use crate::painting::paintable_data::BorderEdge;
use libgfx_rust::{
    CompositingAndBlendingOperator, CornerRadii, FloatMatrix4x4, FloatPoint, FloatRect, FloatSize, IntRect, MaskKind,
    WindingRule, translation_matrix,
};
use scroll_state::{NO_SCROLL_STATE_SLOT, ScrollStateSlot};

pub use crate::painting::display_list::commands::{
    ContextRef, FrameNodeIndex, SpatialNodeIndex, VISUAL_VIEWPORT_NODE_INDEX,
};

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

impl TransformData {
    pub fn matrix_including_origin(&self) -> FloatMatrix4x4 {
        translation_matrix(self.origin.x, self.origin.y, 0.0)
            .multiplied(self.matrix)
            .multiplied(translation_matrix(-self.origin.x, -self.origin.y, 0.0))
    }
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
}

pub struct EffectsData {
    pub opacity: f32,
    pub blend_mode: CompositingAndBlendingOperator,
    pub filter: Option<std::rc::Rc<Vec<u8>>>,
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

impl AnchorScrollShift {
    pub fn masked_offset(&self, scroll_offsets: &[FloatPoint]) -> FloatPoint {
        let mut offset = device_offset_for_index(scroll_offsets, self.scroll_node_index);
        if !self.compensate_horizontal_scroll {
            offset.x = 0.0;
        }
        if !self.compensate_vertical_scroll {
            offset.y = 0.0;
        }
        if self.negate {
            FloatPoint {
                x: -offset.x,
                y: -offset.y,
            }
        } else {
            offset
        }
    }
}

pub fn device_offset_for_index(scroll_offsets: &[FloatPoint], index: SpatialNodeIndex) -> FloatPoint {
    scroll_offsets.get(index.0 as usize).copied().unwrap_or_default()
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IncludeVisualViewportTransform {
    No,
    Yes,
}

struct LocalSpatialMatrix {
    matrix: FloatMatrix4x4,
    flattens_inherited_transform: bool,
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
    pub role: FrameRole,
}

// Marks a spatial node whose content belongs to no 3D rendering context.
pub const NO_SORTING_CONTEXT: SpatialNodeIndex = SpatialNodeIndex(u32::MAX);

// The plane and 3D rendering context that an established context's own plane renders into.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SortingContextLink {
    pub parent_context: SpatialNodeIndex,
    pub parent_leaf: SpatialNodeIndex,
}

// Per-spatial-node 3D rendering context membership: the plane each node's content renders into and the context that
// sorts that plane. A tree without 3D rendering contexts resolves to empty per-node vectors.
#[derive(Default)]
pub struct SortingContexts {
    pub links: HashMap<u32, SortingContextLink>,
    pub leaf_by_node: Vec<SpatialNodeIndex>,
    pub context_by_node: Vec<SpatialNodeIndex>,
}

impl SortingContexts {
    pub fn is_empty(&self) -> bool {
        self.leaf_by_node.is_empty()
    }

    pub fn outermost_context_of(&self, mut context: SpatialNodeIndex) -> SpatialNodeIndex {
        loop {
            let Some(link) = self.links.get(&context.0) else {
                return context;
            };
            if link.parent_context == NO_SORTING_CONTEXT {
                return context;
            }
            context = link.parent_context;
        }
    }
}

pub fn resolve_sorting_contexts_over_nodes(
    node_count: usize,
    parent_and_sorting_context_root_of_node: impl Fn(usize) -> (SpatialNodeIndex, Option<SpatialNodeIndex>),
) -> SortingContexts {
    let mut is_sorting_context_root = vec![false; node_count];
    let mut has_sorting_context_roots = false;
    for index in 0..node_count {
        let (_, sorting_context_root) = parent_and_sorting_context_root_of_node(index);
        if let Some(root) = sorting_context_root {
            is_sorting_context_root[root.0 as usize] = true;
            has_sorting_context_roots = true;
        }
    }
    if !has_sorting_context_roots {
        return SortingContexts::default();
    }

    // Roots always precede their contexts' nodes, so a single forward walk resolves every node.
    let mut contexts = SortingContexts {
        links: HashMap::new(),
        leaf_by_node: Vec::with_capacity(node_count),
        context_by_node: Vec::with_capacity(node_count),
    };
    for (index, is_sorting_context_root) in is_sorting_context_root.iter().copied().enumerate() {
        let spatial_index = SpatialNodeIndex(index as u32);
        let (parent, sorting_context_root) = parent_and_sorting_context_root_of_node(index);
        let inherited_leaf = if index == 0 {
            NO_SORTING_CONTEXT
        } else {
            contexts.leaf_by_node[parent.0 as usize]
        };
        let inherited_context = if index == 0 {
            NO_SORTING_CONTEXT
        } else {
            contexts.context_by_node[parent.0 as usize]
        };
        if let Some(root) = sorting_context_root {
            contexts.leaf_by_node.push(spatial_index);
            contexts.context_by_node.push(root);
        } else if is_sorting_context_root {
            contexts.links.insert(
                spatial_index.0,
                SortingContextLink {
                    parent_context: inherited_context,
                    parent_leaf: inherited_leaf,
                },
            );
            contexts.leaf_by_node.push(spatial_index);
            contexts.context_by_node.push(spatial_index);
        } else {
            contexts.leaf_by_node.push(inherited_leaf);
            contexts.context_by_node.push(inherited_context);
        }
    }
    contexts
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
        if !parent.is_none() {
            let parent_node = &self.frame_nodes[parent.0 as usize];
            debug_assert!(self.spatial_is_ancestor_or_self(parent_node.spatial, spatial));
        }
        self.frame_nodes.push(FrameNode {
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
                        && node.data.kind() == other_node.data.kind()
                })
    }

    fn ancestor_chain(&self, index: SpatialNodeIndex) -> Vec<SpatialNodeIndex> {
        assert!((index.0 as usize) < self.spatial_nodes.len());
        let mut chain = Vec::with_capacity(8);
        let mut current = index;
        loop {
            chain.push(current);
            if current == VISUAL_VIEWPORT_NODE_INDEX {
                break;
            }
            current = self.spatial_nodes[current.0 as usize].parent;
        }
        chain
    }

    fn local_spatial_matrix(&self, index: SpatialNodeIndex, scroll_offsets: &[FloatPoint]) -> LocalSpatialMatrix {
        let translation = |offset: FloatPoint| LocalSpatialMatrix {
            matrix: translation_matrix(offset.x, offset.y, 0.0),
            flattens_inherited_transform: false,
        };
        match &self.spatial_nodes[index.0 as usize].data {
            SpatialData::Transform(transform) => LocalSpatialMatrix {
                matrix: transform.matrix_including_origin(),
                flattens_inherited_transform: transform.flattens_inherited_transform,
            },
            SpatialData::Perspective(perspective) => LocalSpatialMatrix {
                matrix: perspective.matrix,
                flattens_inherited_transform: perspective.flattens_inherited_transform,
            },
            SpatialData::Scroll(_) | SpatialData::Sticky(_) => {
                translation(device_offset_for_index(scroll_offsets, index))
            }
            SpatialData::AnchorScrollShift(shift) => translation(shift.masked_offset(scroll_offsets)),
            SpatialData::BackfaceVisibility(backface) => LocalSpatialMatrix {
                matrix: FloatMatrix4x4::identity(),
                flattens_inherited_transform: backface.flattens_inherited_transform,
            },
        }
    }

    pub fn accumulated_matrix(
        &self,
        index: SpatialNodeIndex,
        scroll_offsets: &[FloatPoint],
        include_visual_viewport_transform: IncludeVisualViewportTransform,
    ) -> FloatMatrix4x4 {
        let chain = self.ancestor_chain(index);
        let mut matrix = FloatMatrix4x4::identity();
        for node_index in chain.into_iter().rev() {
            if node_index == VISUAL_VIEWPORT_NODE_INDEX
                && self.root_is_visual_viewport
                && include_visual_viewport_transform == IncludeVisualViewportTransform::No
            {
                continue;
            }
            let local = self.local_spatial_matrix(node_index, scroll_offsets);
            let inherited = if local.flattens_inherited_transform {
                matrix.flattened()
            } else {
                matrix
            };
            matrix = inherited.multiplied(local.matrix);
        }
        matrix
    }

    pub fn accumulated_2d_scale(
        &self,
        index: SpatialNodeIndex,
        scroll_offsets: &[FloatPoint],
        include_visual_viewport_transform: IncludeVisualViewportTransform,
    ) -> FloatSize {
        let affine = self
            .accumulated_matrix(index, scroll_offsets, include_visual_viewport_transform)
            .extract_2d_affine();
        FloatSize {
            width: affine.x_scale(),
            height: affine.y_scale(),
        }
    }

    pub fn plane_depth_at_point_for_hit_test(
        &self,
        plane_node_index: SpatialNodeIndex,
        screen_point: FloatPoint,
        scroll_offsets: &[FloatPoint],
    ) -> Option<f32> {
        let inverse = self
            .accumulated_matrix(plane_node_index, scroll_offsets, IncludeVisualViewportTransform::Yes)
            .inverse()?;
        let matrix = &inverse.elements;
        let depth = -(screen_point.x * matrix[2][0] + screen_point.y * matrix[2][1] + matrix[2][3]) / matrix[2][2];
        depth.is_finite().then_some(depth)
    }

    pub fn resolve_sorting_contexts(&self) -> SortingContexts {
        resolve_sorting_contexts_over_nodes(self.spatial_nodes.len(), |index| {
            let node = &self.spatial_nodes[index];
            let sorting_context_root = if let SpatialData::Transform(transform) = &node.data {
                transform.sorting_context_root_index
            } else {
                None
            };
            (node.parent, sorting_context_root)
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
            if let Some(bytes) = &effects.filter {
                out.filter_bytes = bytes.as_ptr();
                out.filter_bytes_length = bytes.len();
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

#[cfg(test)]
mod tests {
    use super::*;
    use libgfx_rust::translation_matrix;

    fn transform(translation: f32) -> TransformData {
        TransformData {
            matrix: translation_matrix(translation, translation, 0.0),
            origin: FloatPoint {
                x: translation,
                y: translation,
            },
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
        }
    }

    fn tree() -> VisualContextTree {
        VisualContextTree::create(transform(0.0))
    }

    fn effects() -> FrameData {
        FrameData::Effects(EffectsData {
            opacity: 1.0,
            blend_mode: CompositingAndBlendingOperator::Normal,
            filter: None,
        })
    }

    fn mask(rect: IntRect) -> FrameData {
        FrameData::Mask(MaskData {
            rect,
            kind: MaskKind::Alpha,
            origin: MaskLayerOrigin::CssMaskLayers,
        })
    }

    fn clip(rect: FloatRect, mode: ClipMode) -> FrameData {
        FrameData::Clip(ClipData {
            rect,
            corner_radii: CornerRadii::default(),
            mode,
        })
    }

    fn sorting_transform(root: SpatialNodeIndex) -> TransformData {
        TransformData {
            sorting_context_root_index: Some(root),
            ..transform(0.0)
        }
    }

    #[test]
    fn trees_with_the_same_structure_are_compatible() {
        let mut tree = VisualContextTree::create(transform(1.0));
        tree.append_spatial(SpatialData::Transform(transform(2.0)), VISUAL_VIEWPORT_NODE_INDEX);

        let mut updated_tree = VisualContextTree::create(transform(3.0));
        updated_tree.append_spatial(SpatialData::Transform(transform(4.0)), VISUAL_VIEWPORT_NODE_INDEX);

        assert!(updated_tree.is_compatible_with(&tree));
    }

    #[test]
    fn compatibility_requires_the_same_shape() {
        let mut reference = tree();
        reference.append_spatial(SpatialData::Transform(transform(1.0)), VISUAL_VIEWPORT_NODE_INDEX);

        let shorter_tree = tree();
        assert!(!shorter_tree.is_compatible_with(&reference));

        let mut different_type_tree = tree();
        different_type_tree.append_spatial(
            SpatialData::Perspective(PerspectiveData {
                matrix: FloatMatrix4x4::identity(),
                flattens_inherited_transform: false,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        assert!(!different_type_tree.is_compatible_with(&reference));

        let mut frame_tree = tree();
        frame_tree.append_frame(effects(), FrameNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX);
        assert!(!frame_tree.is_compatible_with(&reference));

        let mut mask_tree = tree();
        mask_tree.append_frame(
            mask(IntRect {
                x: 0,
                y: 0,
                width: 1,
                height: 1,
            }),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        assert!(!mask_tree.is_compatible_with(&frame_tree));

        let mut different_parent_tree = tree();
        let parent =
            different_parent_tree.append_spatial(SpatialData::Transform(transform(1.0)), VISUAL_VIEWPORT_NODE_INDEX);
        different_parent_tree.append_spatial(SpatialData::Transform(transform(2.0)), parent);

        let mut same_node_count_tree = tree();
        same_node_count_tree.append_spatial(SpatialData::Transform(transform(1.0)), VISUAL_VIEWPORT_NODE_INDEX);
        same_node_count_tree.append_spatial(SpatialData::Transform(transform(2.0)), VISUAL_VIEWPORT_NODE_INDEX);
        assert!(!different_parent_tree.is_compatible_with(&same_node_count_tree));

        let mut frame_under_root_tree = tree();
        let frame_under_root_spatial =
            frame_under_root_tree.append_spatial(SpatialData::Transform(transform(1.0)), VISUAL_VIEWPORT_NODE_INDEX);
        frame_under_root_tree.append_frame(effects(), FrameNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX);

        let mut frame_under_transform_tree = tree();
        let frame_under_transform_spatial = frame_under_transform_tree
            .append_spatial(SpatialData::Transform(transform(1.0)), VISUAL_VIEWPORT_NODE_INDEX);
        frame_under_transform_tree.append_frame(effects(), FrameNodeIndex::NONE, frame_under_transform_spatial);

        assert_eq!(frame_under_root_spatial, frame_under_transform_spatial);
        assert!(!frame_under_root_tree.is_compatible_with(&frame_under_transform_tree));
    }

    #[test]
    fn compatibility_requires_the_same_root_isolation_frame() {
        let mut isolated = tree();
        let isolation_frame = isolated.append_frame(effects(), FrameNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX);
        isolated.root_isolation_frame = Some(isolation_frame);

        let mut not_isolated = tree();
        not_isolated.append_frame(effects(), FrameNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX);
        assert!(!isolated.is_compatible_with(&not_isolated));

        let mut also_isolated = tree();
        let also_isolation_frame =
            also_isolated.append_frame(effects(), FrameNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX);
        also_isolated.root_isolation_frame = Some(also_isolation_frame);
        assert!(isolated.is_compatible_with(&also_isolated));
    }

    fn scaled(scale: f32) -> TransformData {
        TransformData {
            matrix: libgfx_rust::scale_matrix(scale, scale, 1.0),
            ..transform(0.0)
        }
    }

    #[test]
    fn accumulated_matrix_composes_the_root_path_and_may_leave_out_the_visual_viewport() {
        let mut tree = VisualContextTree::create(scaled(2.0));
        let scroll = tree.append_spatial(
            SpatialData::Scroll(ScrollData {
                state_slot: NO_SCROLL_STATE_SLOT,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let child = tree.append_spatial(SpatialData::Transform(scaled(3.0)), scroll);
        let scroll_offsets = [FloatPoint::default(), FloatPoint { x: -10.0, y: -20.0 }];

        let without_viewport = tree.accumulated_matrix(child, &scroll_offsets, IncludeVisualViewportTransform::No);
        assert_eq!(
            without_viewport,
            translation_matrix(-10.0, -20.0, 0.0).multiplied(libgfx_rust::scale_matrix(3.0, 3.0, 1.0))
        );
        let with_viewport = tree.accumulated_matrix(child, &scroll_offsets, IncludeVisualViewportTransform::Yes);
        assert_eq!(
            with_viewport,
            libgfx_rust::scale_matrix(2.0, 2.0, 1.0).multiplied(without_viewport)
        );

        assert_eq!(
            tree.accumulated_2d_scale(child, &scroll_offsets, IncludeVisualViewportTransform::No),
            FloatSize {
                width: 3.0,
                height: 3.0
            }
        );
        assert_eq!(
            tree.accumulated_2d_scale(child, &[], IncludeVisualViewportTransform::Yes),
            FloatSize {
                width: 6.0,
                height: 6.0
            }
        );
    }

    #[test]
    fn a_content_root_is_always_included() {
        let mut tree = VisualContextTree::create_with_content_root(scaled(2.0));
        let child = tree.append_spatial(SpatialData::Transform(scaled(3.0)), VISUAL_VIEWPORT_NODE_INDEX);
        assert_eq!(
            tree.accumulated_2d_scale(child, &[], IncludeVisualViewportTransform::No),
            FloatSize {
                width: 6.0,
                height: 6.0
            }
        );
    }

    #[test]
    fn a_transform_origin_does_not_change_the_accumulated_scale() {
        let mut tree = VisualContextTree::create(transform(0.0));
        let child = tree.append_spatial(
            SpatialData::Transform(TransformData {
                origin: FloatPoint { x: 50.0, y: 50.0 },
                ..scaled(2.0)
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let matrix = tree.accumulated_matrix(child, &[], IncludeVisualViewportTransform::No);
        assert_eq!(matrix.map_vector4([50.0, 50.0, 0.0, 1.0]), [50.0, 50.0, 0.0, 1.0]);
        assert_eq!(matrix.map_vector4([0.0, 0.0, 0.0, 1.0]), [-50.0, -50.0, 0.0, 1.0]);
        assert_eq!(
            tree.accumulated_2d_scale(child, &[], IncludeVisualViewportTransform::No),
            FloatSize {
                width: 2.0,
                height: 2.0
            }
        );
    }
    #[test]
    fn a_tree_without_sorting_context_roots_resolves_to_empty_contexts() {
        assert!(tree().resolve_sorting_contexts().is_empty());
    }

    #[test]
    fn sorting_context_membership_is_resolved_for_participants_descendants_and_outsiders() {
        let mut tree = tree();
        let context_root = tree.append_spatial(SpatialData::Transform(transform(0.0)), VISUAL_VIEWPORT_NODE_INDEX);
        let participating_transform =
            tree.append_spatial(SpatialData::Transform(sorting_transform(context_root)), context_root);
        let plain_descendant = tree.append_spatial(SpatialData::Transform(transform(0.0)), participating_transform);
        let outside_context = tree.append_spatial(SpatialData::Transform(transform(0.0)), VISUAL_VIEWPORT_NODE_INDEX);

        let contexts = tree.resolve_sorting_contexts();
        assert!(!contexts.is_empty());
        assert_eq!(contexts.leaf_by_node[context_root.0 as usize], context_root);
        assert_eq!(contexts.context_by_node[context_root.0 as usize], context_root);
        assert_eq!(
            contexts.leaf_by_node[participating_transform.0 as usize],
            participating_transform
        );
        assert_eq!(
            contexts.context_by_node[participating_transform.0 as usize],
            context_root
        );
        assert_eq!(
            contexts.leaf_by_node[plain_descendant.0 as usize],
            participating_transform
        );
        assert_eq!(contexts.context_by_node[plain_descendant.0 as usize], context_root);
        assert_eq!(contexts.leaf_by_node[outside_context.0 as usize], NO_SORTING_CONTEXT);
        assert_eq!(contexts.context_by_node[outside_context.0 as usize], NO_SORTING_CONTEXT);
    }

    #[test]
    fn nested_sorting_contexts_link_to_the_inherited_context_and_leaf() {
        let mut tree = tree();
        let outer_context = tree.append_spatial(SpatialData::Transform(transform(0.0)), VISUAL_VIEWPORT_NODE_INDEX);
        let outer_leaf = tree.append_spatial(SpatialData::Transform(sorting_transform(outer_context)), outer_context);
        let inner_context = tree.append_spatial(SpatialData::Transform(transform(0.0)), outer_leaf);
        tree.append_spatial(SpatialData::Transform(sorting_transform(inner_context)), inner_context);

        let contexts = tree.resolve_sorting_contexts();
        assert_eq!(
            contexts.links.get(&inner_context.0),
            Some(&SortingContextLink {
                parent_context: outer_context,
                parent_leaf: outer_leaf,
            })
        );
        assert_eq!(contexts.outermost_context_of(inner_context), outer_context);
    }

    #[test]
    fn an_untransformed_plane_has_zero_depth() {
        let tree = tree();
        assert_eq!(
            tree.plane_depth_at_point_for_hit_test(VISUAL_VIEWPORT_NODE_INDEX, FloatPoint { x: 25.0, y: 50.0 }, &[]),
            Some(0.0)
        );
    }

    #[test]
    fn a_plane_translated_in_z_has_the_translated_depth() {
        let mut tree = tree();
        let plane = tree.append_spatial(
            SpatialData::Transform(TransformData {
                matrix: translation_matrix(0.0, 0.0, 75.0),
                ..transform(0.0)
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        assert_eq!(
            tree.plane_depth_at_point_for_hit_test(plane, FloatPoint { x: 25.0, y: 50.0 }, &[]),
            Some(75.0)
        );
    }

    #[test]
    fn a_perspective_chain_projects_the_plane_depth() {
        let mut tree = tree();
        let perspective = tree.append_spatial(
            SpatialData::Perspective(PerspectiveData {
                matrix: libgfx_rust::perspective_matrix(1000.0),
                flattens_inherited_transform: false,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let plane = tree.append_spatial(
            SpatialData::Transform(TransformData {
                matrix: translation_matrix(0.0, 0.0, 100.0),
                ..transform(0.0)
            }),
            perspective,
        );
        let depth = tree
            .plane_depth_at_point_for_hit_test(plane, FloatPoint { x: 25.0, y: 50.0 }, &[])
            .unwrap();
        assert!((depth - 1000.0 / 9.0).abs() < 0.0001);
    }

    #[test]
    fn a_singular_accumulated_matrix_has_no_plane_depth() {
        let mut tree = tree();
        let plane = tree.append_spatial(
            SpatialData::Transform(TransformData {
                matrix: libgfx_rust::scale_matrix(0.0, 1.0, 1.0),
                ..transform(0.0)
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        assert_eq!(
            tree.plane_depth_at_point_for_hit_test(plane, FloatPoint { x: 25.0, y: 50.0 }, &[]),
            None
        );
    }
}
