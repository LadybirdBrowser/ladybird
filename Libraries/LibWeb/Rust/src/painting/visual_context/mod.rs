/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub mod basic_shapes;
pub mod box_build;
pub mod build;
pub mod delta;
pub mod dirty;
pub mod dump;
pub mod incremental;
pub mod nested;
pub mod node_values;
pub mod queries;
pub mod reconcile;
pub mod refresh;
pub mod scroll_state;
pub mod serialize;
pub mod shape;

use std::collections::HashMap;
use std::rc::Rc;
use std::sync::atomic::{AtomicU64, Ordering};

use crate::layout::node_data::NodeSlotId;
use libgfx_rust::{
    CompositingAndBlendingOperator, CornerRadii, FloatMatrix4x4, FloatPoint, FloatRect, FloatSize, IntPoint, IntRect,
    MaskKind, WindingRule, translation_matrix,
};
use scroll_state::{NO_SCROLL_STATE_SLOT, ScrollStateSlot};

pub use crate::painting::display_list::commands::{
    ClipMode, ContextRef, FrameNodeIndex, SpatialNodeIndex, VISUAL_VIEWPORT_NODE_INDEX,
};
pub use queries::{ClipBehavior, should_cull_back_face};

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
    pub establishes_sorting_context: bool,
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

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ClipData {
    pub rect: FloatRect,
    pub corner_radii: CornerRadii,
    pub mode: ClipMode,
}

#[derive(Clone)]
pub struct ClipPathData {
    pub path: std::rc::Rc<libgfx_rust::path::OwnedPath>,
    pub bounding_rect: IntRect,
    pub fill_rule: WindingRule,
}

#[derive(Clone)]
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
    pub owner_paintable: NodeSlotId,
    pub registry_parent_node: SpatialNodeIndex,
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
    pub owner_paintable: NodeSlotId,
    pub registry_parent_node: SpatialNodeIndex,
}

impl StickyData {
    pub fn unconstrained(
        scroller: SpatialNodeIndex,
        parent_sticky: Option<SpatialNodeIndex>,
        state_slot: ScrollStateSlot,
        owner_paintable: NodeSlotId,
        registry_parent_node: SpatialNodeIndex,
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
            owner_paintable,
            registry_parent_node,
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

#[derive(Clone)]
pub enum SpatialData {
    Scroll(ScrollData),
    Sticky(StickyData),
    Transform(TransformData),
    Perspective(PerspectiveData),
    BackfaceVisibility(BackfaceVisibilityData),
    AnchorScrollShift(AnchorScrollShift),
    Dead,
}

#[derive(Clone)]
pub enum FrameData {
    Clip(ClipData),
    ClipPath(ClipPathData),
    Effects(EffectsData),
    Mask(MaskData),
    Dead,
}

impl SpatialData {
    pub fn is_live(&self) -> bool {
        !matches!(self, Self::Dead)
    }

    pub fn is_scroll_like(&self) -> bool {
        matches!(self, Self::Scroll(_) | Self::Sticky(_))
    }
}

impl FrameData {
    pub fn clips_everything(&self) -> bool {
        match self {
            Self::Clip(clip) => clip.mode == ClipMode::Intersect && (clip.rect.width <= 0.0 || clip.rect.height <= 0.0),
            Self::ClipPath(clip_path) => {
                let [_, _, width, height] = clip_path.path.bounding_box();
                width <= 0.0 || height <= 0.0
            }
            Self::Effects(_) | Self::Dead => false,
            Self::Mask(mask) => mask.rect.is_empty(),
        }
    }

    pub fn is_live(&self) -> bool {
        !matches!(self, Self::Dead)
    }
}

#[derive(Clone)]
pub struct SpatialNode {
    pub data: SpatialData,
    pub parent: SpatialNodeIndex,
}

#[derive(Clone)]
pub struct FrameNode {
    pub data: FrameData,
    pub parent: FrameNodeIndex,
    pub spatial: SpatialNodeIndex,
    pub clips_everything: bool,
}

impl FrameNode {
    pub fn new(data: FrameData, parent: FrameNodeIndex, spatial: SpatialNodeIndex) -> Self {
        let clips_everything = data.clips_everything();
        Self {
            data,
            parent,
            spatial,
            clips_everything,
        }
    }
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
    nodes_in_dependency_order: &[u32],
    parent_and_sorting_context_root_of_node: impl Fn(usize) -> (SpatialNodeIndex, Option<SpatialNodeIndex>),
) -> SortingContexts {
    let mut is_sorting_context_root = vec![false; node_count];
    let mut has_sorting_context_roots = false;
    for &index in nodes_in_dependency_order {
        let (_, sorting_context_root) = parent_and_sorting_context_root_of_node(index as usize);
        if let Some(root) = sorting_context_root {
            is_sorting_context_root[root.0 as usize] = true;
            has_sorting_context_roots = true;
        }
    }
    if !has_sorting_context_roots {
        return SortingContexts::default();
    }

    // The dependency order lists a node after its parent and its sorting context root, so a single
    // walk along it resolves every node from entries already written.
    let mut contexts = SortingContexts {
        links: HashMap::new(),
        leaf_by_node: vec![NO_SORTING_CONTEXT; node_count],
        context_by_node: vec![NO_SORTING_CONTEXT; node_count],
    };
    for &index in nodes_in_dependency_order {
        let spatial_index = SpatialNodeIndex(index);
        let index = index as usize;
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
            contexts.leaf_by_node[index] = spatial_index;
            contexts.context_by_node[index] = root;
        } else if is_sorting_context_root[index] {
            contexts.links.insert(
                spatial_index.0,
                SortingContextLink {
                    parent_context: inherited_context,
                    parent_leaf: inherited_leaf,
                },
            );
            contexts.leaf_by_node[index] = spatial_index;
            contexts.context_by_node[index] = spatial_index;
        } else {
            contexts.leaf_by_node[index] = inherited_leaf;
            contexts.context_by_node[index] = inherited_context;
        }
    }
    contexts
}

static NEXT_STRUCTURAL_EPOCH: AtomicU64 = AtomicU64::new(1);

pub fn allocate_structural_epoch() -> u64 {
    NEXT_STRUCTURAL_EPOCH.fetch_add(1, Ordering::Relaxed)
}

pub fn resolve_leaf_to_context_matrices(
    contexts: &SortingContexts,
    nodes_in_dependency_order: &[u32],
    parent_by_node: &[SpatialNodeIndex],
    local_matrix_by_node: &[FloatMatrix4x4],
    flattens_inherited_transform_by_node: &[bool],
) -> Vec<FloatMatrix4x4> {
    if contexts.is_empty() {
        return Vec::new();
    }
    let mut matrices = vec![FloatMatrix4x4::identity(); local_matrix_by_node.len()];
    for &index in nodes_in_dependency_order {
        let index = index as usize;
        let context = contexts.context_by_node[index];
        if context.0 as usize == index || index == 0 {
            continue;
        }
        let local_matrix = local_matrix_by_node[index];
        let parent = parent_by_node[index].0 as usize;
        if parent == context.0 as usize {
            matrices[index] = local_matrix;
            continue;
        }
        let mut base = matrices[parent];
        if flattens_inherited_transform_by_node[index] {
            base = base.flattened();
        }
        if context != NO_SORTING_CONTEXT && contexts.context_by_node[parent] != context {
            let root_matrix = matrices[context.0 as usize];
            base = root_matrix
                .inverse()
                .unwrap_or(FloatMatrix4x4::identity())
                .multiplied(base);
        }
        matrices[index] = base.multiplied(local_matrix);
    }
    matrices
}

#[derive(Default)]
pub struct VisualContextState {
    pub tree: Option<Rc<VisualContextTree>>,
    pub paintables_with_mask_nodes: Vec<crate::layout::node_data::NodeSlotId>,
    pub scroll_state: scroll_state::ScrollState,
    pub scroll_state_snapshot: Vec<FloatPoint>,
    pub needs_to_refresh_scroll_state: bool,
    pub build_count: u64,
    pub dirty_boxes: dirty::VisualContextDirtySet,
    pub incremental_update_count: u64,
    pub last_tree_inputs: Option<crate::painting::host::FfiVisualContextTreeInputs>,
    pub last_root_background_source: Option<crate::painting::host::FfiRootBackgroundSource>,
    pub last_full_build_reason: dirty::VisualContextGlobalRebuildReason,
    pub quarantined_slots_are_releasable: bool,
}

impl VisualContextState {
    pub fn structural_epoch(&self) -> u64 {
        self.tree.as_ref().map_or(0, |tree| tree.structural_epoch)
    }

    pub fn release_quarantined_slots_while_no_handle_is_retained(&mut self) {
        if !self.quarantined_slots_are_releasable {
            return;
        }
        self.quarantined_slots_are_releasable = false;
        if let Some(tree) = self.tree.as_mut() {
            Rc::make_mut(tree).release_quarantined_slots_after_recording();
        }
    }
}

#[derive(Clone)]
pub struct VisualContextTree {
    pub spatial_nodes: Vec<SpatialNode>,
    pub frame_nodes: Vec<FrameNode>,
    pub root_is_visual_viewport: bool,
    pub root_isolation_frame: Option<FrameNodeIndex>,
    pub structural_epoch: u64,
    pub live_spatial_node_count: u32,
    pub live_frame_node_count: u32,
    free_spatial_slots: Vec<SpatialNodeIndex>,
    free_frame_slots: Vec<FrameNodeIndex>,
    quarantined_spatial_slots: Vec<SpatialNodeIndex>,
    quarantined_frame_slots: Vec<FrameNodeIndex>,
}

const COMPACTION_DEAD_NODE_THRESHOLD: usize = 512;

pub(crate) struct NodeDependencyOrder {
    pub order: Vec<u32>,
    pub back_edges: Vec<(u32, u32)>,
    pub dangling_references: Vec<(u32, u32)>,
}

fn for_each_spatial_node_reference(
    index: SpatialNodeIndex,
    node: &SpatialNode,
    mut visit: impl FnMut(SpatialNodeIndex),
) {
    if index != VISUAL_VIEWPORT_NODE_INDEX {
        visit(node.parent);
    }
    match &node.data {
        SpatialData::Scroll(scroll) => visit(scroll.registry_parent_node),
        SpatialData::Sticky(sticky) => {
            visit(sticky.scroller);
            if let Some(parent_sticky) = sticky.parent_sticky {
                visit(parent_sticky);
            }
            visit(sticky.registry_parent_node);
        }
        SpatialData::Transform(transform) => {
            if let Some(root) = transform.sorting_context_root_index {
                visit(root);
            }
        }
        SpatialData::BackfaceVisibility(backface) => visit(backface.plane_root_index),
        SpatialData::AnchorScrollShift(shift) => visit(shift.scroll_node_index),
        SpatialData::Perspective(_) | SpatialData::Dead => {}
    }
}

fn dependency_order(
    node_count: usize,
    is_live: impl Fn(usize) -> bool,
    collect_references: impl Fn(usize, &mut Vec<usize>),
) -> NodeDependencyOrder {
    const UNVISITED: u8 = 0;
    const ON_STACK: u8 = 1;
    const DONE: u8 = 2;
    struct PendingNode {
        node: usize,
        references_begin: usize,
        references_end: usize,
        next_reference: usize,
    }
    let mut marks = vec![UNVISITED; node_count];
    let mut order = Vec::with_capacity(node_count);
    let mut back_edges = Vec::new();
    let mut dangling_references = Vec::new();
    let mut references: Vec<usize> = Vec::new();
    let mut stack: Vec<PendingNode> = Vec::new();
    let push = |node: usize, references: &mut Vec<usize>, stack: &mut Vec<PendingNode>, marks: &mut Vec<u8>| {
        marks[node] = ON_STACK;
        let references_begin = references.len();
        collect_references(node, references);
        stack.push(PendingNode {
            node,
            references_begin,
            references_end: references.len(),
            next_reference: references_begin,
        });
    };
    for root in 0..node_count {
        if marks[root] != UNVISITED || !is_live(root) {
            continue;
        }
        push(root, &mut references, &mut stack, &mut marks);
        while let Some(pending) = stack.last_mut() {
            if pending.next_reference == pending.references_end {
                marks[pending.node] = DONE;
                order.push(pending.node as u32);
                references.truncate(pending.references_begin);
                stack.pop();
                continue;
            }
            let referenced = references[pending.next_reference];
            pending.next_reference += 1;
            if referenced >= node_count || !is_live(referenced) {
                dangling_references.push((pending.node as u32, referenced as u32));
                continue;
            }
            match marks[referenced] {
                ON_STACK => back_edges.push((pending.node as u32, referenced as u32)),
                UNVISITED => push(referenced, &mut references, &mut stack, &mut marks),
                _ => {}
            }
        }
    }
    NodeDependencyOrder {
        order,
        back_edges,
        dangling_references,
    }
}

impl VisualContextTree {
    pub fn create(visual_viewport_transform: TransformData) -> Self {
        Self::with_root(visual_viewport_transform, true)
    }

    pub fn create_with_content_root(content_transform: TransformData) -> Self {
        Self::with_root(content_transform, false)
    }

    pub fn create_with_content_offset(content_offset: IntPoint) -> Self {
        Self::create_with_content_root(TransformData {
            matrix: translation_matrix(content_offset.x as f32, content_offset.y as f32, 0.0),
            origin: FloatPoint::default(),
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
        })
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
            structural_epoch: allocate_structural_epoch(),
            live_spatial_node_count: 1,
            live_frame_node_count: 0,
            free_spatial_slots: Vec::new(),
            free_frame_slots: Vec::new(),
            quarantined_spatial_slots: Vec::new(),
            quarantined_frame_slots: Vec::new(),
        }
    }

    pub fn spatial_is_live(&self, index: SpatialNodeIndex) -> bool {
        self.spatial_nodes
            .get(index.0 as usize)
            .is_some_and(|node| node.data.is_live())
    }

    pub fn frame_is_live(&self, index: FrameNodeIndex) -> bool {
        self.frame_nodes
            .get(index.0 as usize)
            .is_some_and(|node| node.data.is_live())
    }

    pub fn dead_node_count(&self) -> usize {
        self.spatial_nodes.len() + self.frame_nodes.len()
            - self.live_spatial_node_count as usize
            - self.live_frame_node_count as usize
    }

    pub fn should_compact(&self) -> bool {
        let live = self.live_spatial_node_count as usize + self.live_frame_node_count as usize;
        self.dead_node_count() > live.max(COMPACTION_DEAD_NODE_THRESHOLD)
    }

    pub fn allocate_spatial_slot(&mut self) -> (SpatialNodeIndex, bool) {
        if let Some(index) = self.free_spatial_slots.pop() {
            debug_assert!(!self.spatial_nodes[index.0 as usize].data.is_live());
            return (index, true);
        }
        self.spatial_nodes.push(SpatialNode {
            data: SpatialData::Dead,
            parent: VISUAL_VIEWPORT_NODE_INDEX,
        });
        (SpatialNodeIndex((self.spatial_nodes.len() - 1) as u32), false)
    }

    pub fn allocate_frame_slot(&mut self) -> (FrameNodeIndex, bool) {
        if let Some(index) = self.free_frame_slots.pop() {
            debug_assert!(!self.frame_nodes[index.0 as usize].data.is_live());
            return (index, true);
        }
        self.frame_nodes.push(FrameNode::new(
            FrameData::Dead,
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        ));
        (FrameNodeIndex((self.frame_nodes.len() - 1) as u32), false)
    }

    pub fn tombstone_spatial_slot(&mut self, index: SpatialNodeIndex) -> bool {
        assert_ne!(
            index, VISUAL_VIEWPORT_NODE_INDEX,
            "the visual viewport node is never tombstoned"
        );
        let node = &mut self.spatial_nodes[index.0 as usize];
        if !node.data.is_live() {
            return false;
        }
        node.data = SpatialData::Dead;
        self.live_spatial_node_count -= 1;
        self.quarantined_spatial_slots.push(index);
        true
    }

    pub fn tombstone_frame_slot(&mut self, index: FrameNodeIndex) -> bool {
        assert_ne!(
            Some(index),
            self.root_isolation_frame,
            "the root isolation frame is never tombstoned"
        );
        let node = &mut self.frame_nodes[index.0 as usize];
        if !node.data.is_live() {
            return false;
        }
        node.data = FrameData::Dead;
        node.clips_everything = false;
        self.live_frame_node_count -= 1;
        self.quarantined_frame_slots.push(index);
        true
    }

    pub fn replace_spatial_node(&mut self, index: SpatialNodeIndex, node: SpatialNode) -> bool {
        debug_assert!(node.data.is_live(), "slots are retired through tombstone_spatial_slot");
        let slot = &mut self.spatial_nodes[index.0 as usize];
        let was_live = slot.data.is_live();
        *slot = node;
        if !was_live {
            self.live_spatial_node_count += 1;
        }
        was_live
    }

    pub fn replace_frame_node(&mut self, index: FrameNodeIndex, node: FrameNode) -> bool {
        debug_assert!(node.data.is_live(), "slots are retired through tombstone_frame_slot");
        let slot = &mut self.frame_nodes[index.0 as usize];
        let was_live = slot.data.is_live();
        *slot = node;
        if !was_live {
            self.live_frame_node_count += 1;
        }
        was_live
    }

    pub fn release_quarantined_slots_after_recording(&mut self) {
        self.free_spatial_slots.append(&mut self.quarantined_spatial_slots);
        self.free_frame_slots.append(&mut self.quarantined_frame_slots);
        self.debug_assert_slot_accounting();
    }

    pub fn free_slot_count(&self) -> usize {
        self.free_spatial_slots.len() + self.free_frame_slots.len()
    }

    pub fn quarantined_slot_count(&self) -> usize {
        self.quarantined_spatial_slots.len() + self.quarantined_frame_slots.len()
    }

    pub(crate) fn debug_assert_slot_accounting(&self) {
        debug_assert_eq!(
            self.dead_node_count(),
            self.free_slot_count() + self.quarantined_slot_count(),
            "every dead slot is either free or quarantined"
        );
    }

    pub fn append_spatial(&mut self, data: SpatialData, parent: SpatialNodeIndex) -> SpatialNodeIndex {
        assert!(
            self.spatial_is_live(parent),
            "a spatial node's parent must be a live node"
        );
        assert!(data.is_live(), "appended spatial nodes are live");
        self.spatial_nodes.push(SpatialNode { data, parent });
        self.live_spatial_node_count += 1;
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
        assert!(
            self.spatial_is_live(spatial),
            "a frame node's spatial node must be a live node"
        );
        assert!(data.is_live(), "appended frame nodes are live");
        if !parent.is_none() {
            assert!(self.frame_is_live(parent), "a frame node's parent must be a live node");
            let parent_node = &self.frame_nodes[parent.0 as usize];
            debug_assert!(self.spatial_is_ancestor_or_self(parent_node.spatial, spatial));
        }
        self.frame_nodes.push(FrameNode::new(data, parent, spatial));
        self.live_frame_node_count += 1;
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

    pub fn set_visual_viewport_matrix_and_origin(&mut self, matrix: FloatMatrix4x4, origin: FloatPoint) {
        let SpatialData::Transform(transform) = &mut self.spatial_nodes[VISUAL_VIEWPORT_NODE_INDEX.0 as usize].data
        else {
            unreachable!("the visual viewport node is a transform");
        };
        transform.matrix = matrix;
        transform.origin = origin;
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
            SpatialData::Dead => LocalSpatialMatrix {
                matrix: FloatMatrix4x4::identity(),
                flattens_inherited_transform: false,
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

    pub(crate) fn spatial_dependency_order_with_back_edges(&self) -> NodeDependencyOrder {
        dependency_order(
            self.spatial_nodes.len(),
            |index| self.spatial_nodes[index].data.is_live(),
            |index, references| {
                for_each_spatial_node_reference(
                    SpatialNodeIndex(index as u32),
                    &self.spatial_nodes[index],
                    |referenced| {
                        references.push(referenced.0 as usize);
                    },
                );
            },
        )
    }

    pub(crate) fn frame_dependency_order_with_back_edges(&self) -> NodeDependencyOrder {
        dependency_order(
            self.frame_nodes.len(),
            |index| self.frame_nodes[index].data.is_live(),
            |index, references| {
                let parent = self.frame_nodes[index].parent;
                if !parent.is_none() {
                    references.push(parent.0 as usize);
                }
            },
        )
    }

    pub fn spatial_dependency_order(&self) -> Vec<u32> {
        let dependency_order = self.spatial_dependency_order_with_back_edges();
        debug_assert!(
            dependency_order.back_edges.is_empty() && dependency_order.dangling_references.is_empty(),
            "spatial node references form a cycle or dangle: {:?} {:?}",
            dependency_order.back_edges,
            dependency_order.dangling_references
        );
        dependency_order.order
    }

    pub fn frame_dependency_order(&self) -> Vec<u32> {
        let dependency_order = self.frame_dependency_order_with_back_edges();
        debug_assert!(
            dependency_order.back_edges.is_empty() && dependency_order.dangling_references.is_empty(),
            "frame node parents form a cycle or dangle: {:?} {:?}",
            dependency_order.back_edges,
            dependency_order.dangling_references
        );
        dependency_order.order
    }

    pub fn resolve_sorting_contexts(&self) -> SortingContexts {
        self.resolve_sorting_contexts_in_order(&self.spatial_dependency_order())
    }

    pub fn resolve_sorting_contexts_in_order(&self, nodes_in_dependency_order: &[u32]) -> SortingContexts {
        resolve_sorting_contexts_over_nodes(self.spatial_nodes.len(), nodes_in_dependency_order, |index| {
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

pub trait VisualContextNodeSink {
    fn append_spatial_node(&mut self, data: SpatialData, parent: SpatialNodeIndex) -> SpatialNodeIndex;
    fn append_frame_node(
        &mut self,
        data: FrameData,
        parent: FrameNodeIndex,
        spatial: SpatialNodeIndex,
    ) -> FrameNodeIndex;
    fn spatial_node_at(&self, index: SpatialNodeIndex) -> &SpatialNode;
    fn next_spatial_node_index(&self) -> SpatialNodeIndex;
    fn next_frame_node_index(&self) -> FrameNodeIndex;

    fn append_spatial_node_under(&mut self, context: ContextRef, data: SpatialData) -> ContextRef {
        ContextRef {
            spatial: self.append_spatial_node(data, context.spatial),
            ..context
        }
    }

    fn append_frame_node_under(&mut self, context: ContextRef, data: FrameData) -> ContextRef {
        ContextRef {
            frame: self.append_frame_node(data, context.frame, context.spatial),
            ..context
        }
    }
}

impl VisualContextNodeSink for VisualContextTree {
    fn append_spatial_node(&mut self, data: SpatialData, parent: SpatialNodeIndex) -> SpatialNodeIndex {
        self.append_spatial(data, parent)
    }

    fn append_frame_node(
        &mut self,
        data: FrameData,
        parent: FrameNodeIndex,
        spatial: SpatialNodeIndex,
    ) -> FrameNodeIndex {
        self.append_frame(data, parent, spatial)
    }

    fn spatial_node_at(&self, index: SpatialNodeIndex) -> &SpatialNode {
        &self.spatial_nodes[index.0 as usize]
    }

    fn next_spatial_node_index(&self) -> SpatialNodeIndex {
        SpatialNodeIndex(self.spatial_nodes.len() as u32)
    }

    fn next_frame_node_index(&self) -> FrameNodeIndex {
        FrameNodeIndex(self.frame_nodes.len() as u32)
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct BoxVisualContextNodeHandles {
    pub spatial: Vec<SpatialNodeIndex>,
    pub chain_frames: Vec<FrameNodeIndex>,
    pub descendant_frames: Vec<FrameNodeIndex>,
}

pub static EMPTY_BOX_VISUAL_CONTEXT_NODE_HANDLES: BoxVisualContextNodeHandles = BoxVisualContextNodeHandles {
    spatial: Vec::new(),
    chain_frames: Vec::new(),
    descendant_frames: Vec::new(),
};

impl BoxVisualContextNodeHandles {
    pub fn frame_handles(&self) -> impl Iterator<Item = FrameNodeIndex> + '_ {
        self.chain_frames.iter().chain(&self.descendant_frames).copied()
    }

    pub fn contains_frame(&self, frame: FrameNodeIndex) -> bool {
        self.frame_handles().any(|handle| handle == frame)
    }
}

// Nearest ancestor scroll node resolved along the containing block chain, drilled down alongside
// the visual context indices. A fixed-position ancestor decouples its subtree from all outer
// scrollers, but sticky boxes must still reference a scrollport through fixed-position ancestors
// for their sticky offset computation, so both resolutions are carried.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct NearestScrollNodeIndices {
    pub stopping_at_fixed_position_ancestors: SpatialNodeIndex,
    pub continuing_through_fixed_position_ancestors: SpatialNodeIndex,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct DescendantVisualContexts {
    pub normal: ContextRef,
    pub absolute_position: ContextRef,
    pub fixed_position: ContextRef,
    pub normal_nearest_scroll_nodes: NearestScrollNodeIndices,
    pub absolute_position_nearest_scroll_nodes: NearestScrollNodeIndices,
    pub fixed_position_nearest_scroll_nodes: NearestScrollNodeIndices,
    pub normal_plane_root: SpatialNodeIndex,
    pub absolute_position_plane_root: SpatialNodeIndex,
    pub fixed_position_plane_root: SpatialNodeIndex,
    pub flattens_inherited_transform: bool,
    pub sorting_context_root: Option<SpatialNodeIndex>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct PaintableVisualContextRecord {
    pub inherited_input: DescendantVisualContexts,
    pub output_for_descendants: DescendantVisualContexts,
    pub node_handles: BoxVisualContextNodeHandles,
    pub has_mask_nodes: bool,
    pub may_be_root_element: bool,
    pub owns_geometry_dependent_nodes: bool,
    pub subtree_may_own_geometry_dependent_nodes: bool,
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::node_data::NodeSlotId;
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
            establishes_sorting_context: false,
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
                owner_paintable: NodeSlotId::INVALID,
                registry_parent_node: VISUAL_VIEWPORT_NODE_INDEX,
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

    fn viewport_scroll() -> SpatialData {
        SpatialData::Scroll(ScrollData {
            state_slot: NO_SCROLL_STATE_SLOT,
            owner_paintable: NodeSlotId::INVALID,
            registry_parent_node: VISUAL_VIEWPORT_NODE_INDEX,
        })
    }

    #[test]
    fn spatial_dependency_order_puts_every_reference_before_its_referrer() {
        let mut tree = tree();
        let scroll = tree.append_spatial(viewport_scroll(), VISUAL_VIEWPORT_NODE_INDEX);
        let child = tree.append_spatial(SpatialData::Transform(transform(0.0)), VISUAL_VIEWPORT_NODE_INDEX);
        let parent = tree.append_spatial(SpatialData::Transform(transform(0.0)), scroll);
        tree.spatial_nodes[child.0 as usize].parent = parent;
        let sticky = tree.append_spatial(
            SpatialData::Sticky(StickyData::unconstrained(
                scroll,
                None,
                NO_SCROLL_STATE_SLOT,
                NodeSlotId::INVALID,
                scroll,
            )),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let order = tree.spatial_dependency_order();
        let position = |index: SpatialNodeIndex| order.iter().position(|entry| *entry == index.0).unwrap();
        assert_eq!(order.len(), 5);
        assert!(position(parent) < position(child));
        assert!(position(scroll) < position(parent));
        assert!(position(scroll) < position(sticky));
        assert_eq!(order[0], VISUAL_VIEWPORT_NODE_INDEX.0);
    }

    #[test]
    fn frame_dependency_order_follows_parents_only() {
        let mut tree = tree();
        let root_isolation_frame = tree.append_frame(effects(), FrameNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX);
        let child = tree.append_frame(effects(), root_isolation_frame, VISUAL_VIEWPORT_NODE_INDEX);
        let parent = tree.append_frame(effects(), root_isolation_frame, VISUAL_VIEWPORT_NODE_INDEX);
        tree.frame_nodes[child.0 as usize].parent = parent;
        assert_eq!(
            tree.frame_dependency_order(),
            vec![root_isolation_frame.0, parent.0, child.0]
        );
    }

    #[test]
    fn a_reference_cycle_and_a_self_reference_are_reported_as_back_edges() {
        let mut tree = tree();
        let first = tree.append_spatial(SpatialData::Transform(transform(0.0)), VISUAL_VIEWPORT_NODE_INDEX);
        let second = tree.append_spatial(SpatialData::Transform(transform(0.0)), first);
        tree.spatial_nodes[first.0 as usize].parent = second;
        let order = tree.spatial_dependency_order_with_back_edges();
        assert_eq!(order.back_edges, vec![(second.0, first.0)]);
        assert!(order.dangling_references.is_empty());
        assert_eq!(order.order.len(), 3);

        let mut self_referencing = self::tree();
        let anchored = self_referencing.append_spatial(
            SpatialData::AnchorScrollShift(AnchorScrollShift {
                scroll_node_index: SpatialNodeIndex(1),
                negate: false,
                compensate_horizontal_scroll: true,
                compensate_vertical_scroll: true,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let order = self_referencing.spatial_dependency_order_with_back_edges();
        assert_eq!(order.back_edges, vec![(anchored.0, anchored.0)]);
    }

    #[test]
    fn a_reference_out_of_range_is_reported_as_dangling() {
        let mut tree = tree();
        let marker = tree.append_spatial(
            SpatialData::BackfaceVisibility(BackfaceVisibilityData {
                plane_root_index: SpatialNodeIndex(9),
                flattens_inherited_transform: false,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let order = tree.spatial_dependency_order_with_back_edges();
        assert_eq!(order.dangling_references, vec![(marker.0, 9)]);
        assert!(order.back_edges.is_empty());
    }

    #[test]
    fn sorting_contexts_resolve_the_same_for_a_child_stored_below_its_parent() {
        let mut in_order = tree();
        let context_root = in_order.append_spatial(SpatialData::Transform(transform(0.0)), VISUAL_VIEWPORT_NODE_INDEX);
        let participant =
            in_order.append_spatial(SpatialData::Transform(sorting_transform(context_root)), context_root);
        let descendant = in_order.append_spatial(SpatialData::Transform(transform(0.0)), participant);

        let mut permuted = tree();
        let permuted_descendant =
            permuted.append_spatial(SpatialData::Transform(transform(0.0)), VISUAL_VIEWPORT_NODE_INDEX);
        let permuted_participant =
            permuted.append_spatial(SpatialData::Transform(transform(0.0)), VISUAL_VIEWPORT_NODE_INDEX);
        let permuted_context_root =
            permuted.append_spatial(SpatialData::Transform(transform(0.0)), VISUAL_VIEWPORT_NODE_INDEX);
        permuted.spatial_nodes[permuted_participant.0 as usize] = SpatialNode {
            data: SpatialData::Transform(sorting_transform(permuted_context_root)),
            parent: permuted_context_root,
        };
        permuted.spatial_nodes[permuted_descendant.0 as usize].parent = permuted_participant;

        let expected = in_order.resolve_sorting_contexts();
        let contexts = permuted.resolve_sorting_contexts();
        let pairs = [
            (context_root, permuted_context_root),
            (participant, permuted_participant),
            (descendant, permuted_descendant),
        ];
        let map = |index: SpatialNodeIndex| {
            pairs
                .iter()
                .find(|(original, _)| *original == index)
                .map_or(index, |(_, mapped)| *mapped)
        };
        for (original, mapped) in pairs {
            assert_eq!(
                contexts.leaf_by_node[mapped.0 as usize],
                map(expected.leaf_by_node[original.0 as usize])
            );
            assert_eq!(
                contexts.context_by_node[mapped.0 as usize],
                map(expected.context_by_node[original.0 as usize])
            );
        }
        assert_eq!(
            contexts.outermost_context_of(permuted_context_root),
            permuted_context_root
        );
    }

    #[test]
    fn tombstones_keep_their_links_and_leave_the_live_counts() {
        let mut tree = tree();
        let transform = tree.append_spatial(SpatialData::Transform(transform(0.0)), VISUAL_VIEWPORT_NODE_INDEX);
        let empty_clip = tree.append_frame(
            clip(FloatRect::new(0.0, 0.0, 0.0, 0.0), ClipMode::Intersect),
            FrameNodeIndex::NONE,
            transform,
        );
        assert_eq!(tree.live_spatial_node_count, 2);
        assert_eq!(tree.live_frame_node_count, 1);
        assert!(tree.frame_nodes[empty_clip.0 as usize].clips_everything);
        assert!(tree.tombstone_frame_slot(empty_clip));
        assert!(tree.tombstone_spatial_slot(transform));
        assert!(!tree.tombstone_spatial_slot(transform));
        assert!(!tree.spatial_is_live(transform));
        assert!(!tree.frame_is_live(empty_clip));
        assert_eq!(
            tree.spatial_nodes[transform.0 as usize].parent,
            VISUAL_VIEWPORT_NODE_INDEX
        );
        assert!(!tree.frame_nodes[empty_clip.0 as usize].clips_everything);
        assert_eq!(tree.live_spatial_node_count, 1);
        assert_eq!(tree.live_frame_node_count, 0);
        assert_eq!(tree.dead_node_count(), 2);
        assert!(!tree.should_compact());
    }

    #[test]
    fn tombstoned_slots_stay_quarantined_until_a_recording_completes() {
        let mut tree = tree();
        let tombstoned = tree.append_spatial(SpatialData::Transform(transform(0.0)), VISUAL_VIEWPORT_NODE_INDEX);
        assert!(tree.tombstone_spatial_slot(tombstoned));
        assert_eq!(tree.quarantined_slot_count(), 1);
        assert_eq!(tree.free_slot_count(), 0);
        let (fresh, reused) = tree.allocate_spatial_slot();
        assert!(!reused);
        assert_eq!(fresh, SpatialNodeIndex(2));
        assert!(!tree.replace_spatial_node(
            fresh,
            SpatialNode {
                data: SpatialData::Transform(transform(0.0)),
                parent: VISUAL_VIEWPORT_NODE_INDEX,
            },
        ));
        tree.release_quarantined_slots_after_recording();
        assert_eq!(tree.quarantined_slot_count(), 0);
        assert_eq!(tree.free_slot_count(), 1);
        let (recycled, reused) = tree.allocate_spatial_slot();
        assert!(reused);
        assert_eq!(recycled, tombstoned);
    }

    #[test]
    fn released_slots_are_reused_before_the_arrays_grow() {
        let mut tree = tree();
        let first = tree.append_frame(effects(), FrameNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX);
        let second = tree.append_frame(effects(), FrameNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX);
        assert!(tree.tombstone_frame_slot(first));
        assert!(tree.tombstone_frame_slot(second));
        tree.release_quarantined_slots_after_recording();
        let frame_count = tree.frame_nodes.len();
        assert_eq!(tree.allocate_frame_slot(), (second, true));
        assert_eq!(tree.allocate_frame_slot(), (first, true));
        assert_eq!(tree.frame_nodes.len(), frame_count);
        assert_eq!(tree.allocate_frame_slot(), (FrameNodeIndex(frame_count as u32), false));
    }

    #[test]
    fn dead_node_count_counts_free_and_quarantined_slots() {
        let mut tree = tree();
        let transform_node = tree.append_spatial(SpatialData::Transform(transform(0.0)), VISUAL_VIEWPORT_NODE_INDEX);
        let clip_frame = tree.append_frame(
            clip(FloatRect::new(0.0, 0.0, 1.0, 1.0), ClipMode::Intersect),
            FrameNodeIndex::NONE,
            transform_node,
        );
        assert!(tree.tombstone_frame_slot(clip_frame));
        tree.release_quarantined_slots_after_recording();
        assert!(tree.tombstone_spatial_slot(transform_node));
        assert_eq!(tree.dead_node_count(), 2);
        assert_eq!(tree.free_slot_count(), 1);
        assert_eq!(tree.quarantined_slot_count(), 1);
        let (slot, reused) = tree.allocate_frame_slot();
        assert!(reused);
        assert!(!tree.replace_frame_node(
            slot,
            FrameNode::new(effects(), FrameNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX)
        ));
        assert_eq!(tree.dead_node_count(), 1);
        tree.debug_assert_slot_accounting();
    }

    #[test]
    fn dependency_orders_leave_tombstones_out() {
        let mut tree = tree();
        let stale = tree.append_spatial(SpatialData::Transform(transform(0.0)), VISUAL_VIEWPORT_NODE_INDEX);
        let other = tree.append_spatial(SpatialData::Transform(transform(0.0)), VISUAL_VIEWPORT_NODE_INDEX);
        let frame = tree.append_frame(effects(), FrameNodeIndex::NONE, other);
        assert!(tree.tombstone_spatial_slot(stale));
        assert!(tree.tombstone_frame_slot(frame));
        let order = tree.spatial_dependency_order_with_back_edges();
        assert_eq!(order.order, vec![VISUAL_VIEWPORT_NODE_INDEX.0, other.0]);
        assert!(order.back_edges.is_empty());
        assert!(order.dangling_references.is_empty());
        assert!(tree.frame_dependency_order().is_empty());
    }

    #[test]
    fn a_live_node_referencing_a_tombstone_dangles() {
        let mut tree = tree();
        let plane_root = tree.append_spatial(SpatialData::Transform(transform(0.0)), VISUAL_VIEWPORT_NODE_INDEX);
        let marker = tree.append_spatial(
            SpatialData::BackfaceVisibility(BackfaceVisibilityData {
                plane_root_index: plane_root,
                flattens_inherited_transform: false,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        assert!(tree.tombstone_spatial_slot(plane_root));
        let order = tree.spatial_dependency_order_with_back_edges();
        assert_eq!(order.dangling_references, vec![(marker.0, plane_root.0)]);
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

    fn resolve_matrices(parents: &[u32], roots: &[Option<u32>], locals: &[FloatMatrix4x4]) -> Vec<FloatMatrix4x4> {
        let dependency_order: Vec<u32> = (0..parents.len() as u32).collect();
        resolve_matrices_in_order(parents, roots, locals, &dependency_order)
    }

    fn resolve_matrices_in_order(
        parents: &[u32],
        roots: &[Option<u32>],
        locals: &[FloatMatrix4x4],
        dependency_order: &[u32],
    ) -> Vec<FloatMatrix4x4> {
        let contexts = resolve_sorting_contexts_over_nodes(parents.len(), dependency_order, |index| {
            (SpatialNodeIndex(parents[index]), roots[index].map(SpatialNodeIndex))
        });
        let parent_by_node: Vec<SpatialNodeIndex> = parents.iter().map(|parent| SpatialNodeIndex(*parent)).collect();
        resolve_leaf_to_context_matrices(
            &contexts,
            dependency_order,
            &parent_by_node,
            locals,
            &vec![false; parents.len()],
        )
    }

    #[test]
    fn leaf_to_context_matrices_compose_from_the_context_root() {
        let locals = [
            FloatMatrix4x4::identity(),
            translation_matrix(1.0, 0.0, 0.0),
            translation_matrix(0.0, 2.0, 0.0),
            translation_matrix(0.0, 0.0, 3.0),
        ];
        let matrices = resolve_matrices(&[0, 0, 1, 2], &[None, None, Some(1), None], &locals);
        assert_eq!(matrices[1], FloatMatrix4x4::identity());
        assert_eq!(matrices[2], locals[2]);
        assert_eq!(matrices[3], locals[2].multiplied(locals[3]));
    }

    #[test]
    fn leaf_to_context_matrices_rebase_through_a_nested_context_root() {
        let locals = [
            FloatMatrix4x4::identity(),
            translation_matrix(1.0, 0.0, 0.0),
            translation_matrix(0.0, 2.0, 0.0),
            translation_matrix(0.0, 0.0, 3.0),
            translation_matrix(4.0, 0.0, 0.0),
        ];
        let matrices = resolve_matrices(&[0, 0, 1, 2, 3], &[None, None, Some(1), None, Some(2)], &locals);
        assert_eq!(matrices[2], locals[2]);
        assert_eq!(matrices[3], locals[2].multiplied(locals[3]));
        assert_eq!(matrices[4], locals[3].multiplied(locals[4]));
    }

    #[test]
    fn leaf_to_context_matrices_compose_over_a_context_root_at_a_higher_index() {
        let locals = [
            FloatMatrix4x4::identity(),
            translation_matrix(0.0, 0.0, 3.0),
            translation_matrix(0.0, 2.0, 0.0),
            translation_matrix(1.0, 0.0, 0.0),
        ];
        let matrices = resolve_matrices_in_order(&[0, 2, 3, 0], &[None, None, Some(3), None], &locals, &[0, 3, 2, 1]);
        assert_eq!(matrices[3], FloatMatrix4x4::identity());
        assert_eq!(matrices[2], locals[2]);
        assert_eq!(matrices[1], locals[2].multiplied(locals[1]));
    }

    #[test]
    fn leaf_to_context_matrices_leave_a_tombstoned_slot_at_identity() {
        let locals = [
            FloatMatrix4x4::identity(),
            translation_matrix(0.0, 0.0, 3.0),
            translation_matrix(0.0, 2.0, 0.0),
            translation_matrix(1.0, 0.0, 0.0),
        ];
        let matrices = resolve_matrices_in_order(&[0, 2, 3, 0], &[None, None, Some(3), None], &locals, &[0, 3, 2]);
        assert_eq!(matrices[1], FloatMatrix4x4::identity());
        assert_eq!(matrices[2], locals[2]);
    }
}
