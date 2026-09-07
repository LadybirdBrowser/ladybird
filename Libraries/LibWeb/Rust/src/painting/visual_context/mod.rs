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
    ClipMode, ClipNodeIndex, ContextRef, EffectNodeIndex, SpatialNodeIndex, VISUAL_VIEWPORT_NODE_INDEX,
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

impl EffectNodeData {
    pub fn layer_blending_with(blend_mode: CompositingAndBlendingOperator) -> Self {
        EffectNodeData::Effects(EffectsData {
            opacity: 1.0,
            blend_mode,
            filter: None,
        })
    }
}

impl ClipNodeData {
    pub fn rect_clip(rect: FloatRect) -> Self {
        ClipNodeData::Rect(ClipData {
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

// The clip tree holds what narrows the painted region without a layer: rectangle (possibly rounded,
// possibly subtractive) clips and clip paths. The effect tree holds what needs a layer or a
// per-box marker: opacity/blend/filter layers, masks and compositor background-color animations.
#[derive(Clone)]
pub enum ClipNodeData {
    Rect(ClipData),
    Path(ClipPathData),
    Dead,
}

#[derive(Clone)]
pub enum EffectNodeData {
    Effects(EffectsData),
    Mask(MaskData),
    BackgroundColorAnimation,
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

impl ClipNodeData {
    pub fn clips_everything(&self) -> bool {
        match self {
            Self::Rect(clip) => clip.mode == ClipMode::Intersect && (clip.rect.width <= 0.0 || clip.rect.height <= 0.0),
            Self::Path(clip_path) => {
                let [_, _, width, height] = clip_path.path.bounding_box();
                width <= 0.0 || height <= 0.0
            }
            Self::Dead => false,
        }
    }

    pub fn is_live(&self) -> bool {
        !matches!(self, Self::Dead)
    }
}

impl EffectNodeData {
    pub fn is_live(&self) -> bool {
        !matches!(self, Self::Dead)
    }

    pub fn pushes_layer(&self) -> bool {
        matches!(self, Self::Effects(_) | Self::Mask(_))
    }

    // An empty mask leaves nothing of the content it wraps.
    pub fn culls_everything(&self) -> bool {
        matches!(self, Self::Mask(mask) if mask.rect.is_empty())
    }
}

#[derive(Clone)]
pub struct SpatialNode {
    pub data: SpatialData,
    pub parent: SpatialNodeIndex,
}

#[derive(Clone)]
pub struct ClipNode {
    pub data: ClipNodeData,
    pub parent: ClipNodeIndex,
    pub spatial: SpatialNodeIndex,
    pub clips_everything: bool,
}

impl ClipNode {
    pub fn new(data: ClipNodeData, parent: ClipNodeIndex, spatial: SpatialNodeIndex) -> Self {
        let clips_everything = data.clips_everything();
        Self {
            data,
            parent,
            spatial,
            clips_everything,
        }
    }
}

// An effect's layer is pushed inside `output_clip`, the deepest clip shared by every context that
// records under the effect; the clips below it are pushed inside the layer.
#[derive(Clone)]
pub struct EffectNode {
    pub data: EffectNodeData,
    pub parent: EffectNodeIndex,
    pub spatial: SpatialNodeIndex,
    // New effects have no resolved clip until finalization. Rebuilt effects retain their
    // previous result so finalization can compare the old and new layer placement.
    resolved_output_clip: Option<ClipNodeIndex>,
}

impl EffectNode {
    pub fn new(
        data: EffectNodeData,
        parent: EffectNodeIndex,
        spatial: SpatialNodeIndex,
        resolved_output_clip: Option<ClipNodeIndex>,
    ) -> Self {
        Self {
            data,
            parent,
            spatial,
            resolved_output_clip,
        }
    }

    pub fn output_clip(&self) -> ClipNodeIndex {
        self.resolved_output_clip
            .expect("an effect's output clip is resolved before use")
    }
}

// The slot lifecycle every node kind shares: a slot is allocated from the free list or by growing
// the array, retired into quarantine until the recording that may still name it completes, and
// released to the free list afterwards.
pub trait SlotNode: Sized {
    type Index: Copy + PartialEq + std::fmt::Debug;
    fn index(raw: u32) -> Self::Index;
    fn raw(index: Self::Index) -> u32;
    fn is_live(&self) -> bool;
    fn dead() -> Self;
    fn tombstone(&mut self);
}

#[derive(Clone)]
struct SlotAccounting<Index> {
    live_count: u32,
    free: Vec<Index>,
    quarantined: Vec<Index>,
}

impl<Index> Default for SlotAccounting<Index> {
    fn default() -> Self {
        Self {
            live_count: 0,
            free: Vec::new(),
            quarantined: Vec::new(),
        }
    }
}

impl<Index: Copy> SlotAccounting<Index> {
    fn release_quarantined(&mut self) {
        self.free.append(&mut self.quarantined);
    }
}

fn allocate_slot<N: SlotNode>(nodes: &mut Vec<N>, accounting: &mut SlotAccounting<N::Index>) -> (N::Index, bool) {
    if let Some(index) = accounting.free.pop() {
        debug_assert!(!nodes[N::raw(index) as usize].is_live());
        return (index, true);
    }
    nodes.push(N::dead());
    (N::index((nodes.len() - 1) as u32), false)
}

fn tombstone_slot<N: SlotNode>(nodes: &mut [N], accounting: &mut SlotAccounting<N::Index>, index: N::Index) -> bool {
    let node = &mut nodes[N::raw(index) as usize];
    if !node.is_live() {
        return false;
    }
    node.tombstone();
    accounting.live_count -= 1;
    accounting.quarantined.push(index);
    true
}

fn replace_slot<N: SlotNode>(
    nodes: &mut [N],
    accounting: &mut SlotAccounting<N::Index>,
    index: N::Index,
    node: N,
) -> bool {
    debug_assert!(node.is_live(), "slots are retired by tombstoning");
    let slot = &mut nodes[N::raw(index) as usize];
    let was_live = slot.is_live();
    *slot = node;
    if !was_live {
        accounting.live_count += 1;
    }
    was_live
}

impl SlotNode for SpatialNode {
    type Index = SpatialNodeIndex;
    fn index(raw: u32) -> SpatialNodeIndex {
        SpatialNodeIndex(raw)
    }
    fn raw(index: SpatialNodeIndex) -> u32 {
        index.0
    }
    fn is_live(&self) -> bool {
        self.data.is_live()
    }
    fn dead() -> Self {
        Self {
            data: SpatialData::Dead,
            parent: VISUAL_VIEWPORT_NODE_INDEX,
        }
    }
    fn tombstone(&mut self) {
        self.data = SpatialData::Dead;
    }
}

impl SlotNode for ClipNode {
    type Index = ClipNodeIndex;
    fn index(raw: u32) -> ClipNodeIndex {
        ClipNodeIndex(raw)
    }
    fn raw(index: ClipNodeIndex) -> u32 {
        index.0
    }
    fn is_live(&self) -> bool {
        self.data.is_live()
    }
    fn dead() -> Self {
        Self::new(ClipNodeData::Dead, ClipNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX)
    }
    fn tombstone(&mut self) {
        self.data = ClipNodeData::Dead;
        self.clips_everything = false;
    }
}

impl SlotNode for EffectNode {
    type Index = EffectNodeIndex;
    fn index(raw: u32) -> EffectNodeIndex {
        EffectNodeIndex(raw)
    }
    fn raw(index: EffectNodeIndex) -> u32 {
        index.0
    }
    fn is_live(&self) -> bool {
        self.data.is_live()
    }
    fn dead() -> Self {
        Self::new(
            EffectNodeData::Dead,
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            Some(ClipNodeIndex::NONE),
        )
    }
    fn tombstone(&mut self) {
        self.data = EffectNodeData::Dead;
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
    pub clip_nodes: Vec<ClipNode>,
    pub effect_nodes: Vec<EffectNode>,
    pub root_is_visual_viewport: bool,
    // The layer at the root of the effect tree that 3D plane clips are pushed right above.
    pub root_isolation_effect: Option<EffectNodeIndex>,
    pub structural_epoch: u64,
    spatial_slots: SlotAccounting<SpatialNodeIndex>,
    clip_slots: SlotAccounting<ClipNodeIndex>,
    effect_slots: SlotAccounting<EffectNodeIndex>,
    // Keyed by effect node index.
    sampled_background_colors: HashMap<u32, libgfx_rust::Color>,
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
            clip_nodes: Vec::new(),
            effect_nodes: Vec::new(),
            root_is_visual_viewport,
            root_isolation_effect: None,
            structural_epoch: allocate_structural_epoch(),
            spatial_slots: SlotAccounting {
                live_count: 1,
                ..SlotAccounting::default()
            },
            clip_slots: SlotAccounting::default(),
            effect_slots: SlotAccounting::default(),
            sampled_background_colors: HashMap::new(),
        }
    }

    // A tree over decoded arrays: no slot is free or quarantined, the live counts are recounted.
    pub(crate) fn from_nodes(
        spatial_nodes: Vec<SpatialNode>,
        clip_nodes: Vec<ClipNode>,
        effect_nodes: Vec<EffectNode>,
        root_is_visual_viewport: bool,
        root_isolation_effect: Option<EffectNodeIndex>,
        structural_epoch: u64,
    ) -> Self {
        fn accounting<N: SlotNode>(nodes: &[N]) -> SlotAccounting<N::Index> {
            SlotAccounting {
                live_count: nodes.iter().filter(|node| node.is_live()).count() as u32,
                ..SlotAccounting::default()
            }
        }
        Self {
            spatial_slots: accounting(&spatial_nodes),
            clip_slots: accounting(&clip_nodes),
            effect_slots: accounting(&effect_nodes),
            spatial_nodes,
            clip_nodes,
            effect_nodes,
            root_is_visual_viewport,
            root_isolation_effect,
            structural_epoch,
            sampled_background_colors: HashMap::new(),
        }
    }

    pub fn spatial_is_live(&self, index: SpatialNodeIndex) -> bool {
        self.spatial_nodes
            .get(index.0 as usize)
            .is_some_and(|node| node.data.is_live())
    }

    pub fn clip_is_live(&self, index: ClipNodeIndex) -> bool {
        self.clip_nodes
            .get(index.0 as usize)
            .is_some_and(|node| node.data.is_live())
    }

    pub fn effect_is_live(&self, index: EffectNodeIndex) -> bool {
        self.effect_nodes
            .get(index.0 as usize)
            .is_some_and(|node| node.data.is_live())
    }

    // The absent node passes for live where a reference may be absent.
    pub fn clip_is_none_or_live(&self, index: ClipNodeIndex) -> bool {
        index.is_none() || self.clip_is_live(index)
    }

    pub fn effect_is_none_or_live(&self, index: EffectNodeIndex) -> bool {
        index.is_none() || self.effect_is_live(index)
    }

    pub fn live_spatial_node_count(&self) -> u32 {
        self.spatial_slots.live_count
    }

    pub fn live_clip_node_count(&self) -> u32 {
        self.clip_slots.live_count
    }

    pub fn live_effect_node_count(&self) -> u32 {
        self.effect_slots.live_count
    }

    pub fn live_node_count(&self) -> usize {
        (self.spatial_slots.live_count + self.clip_slots.live_count + self.effect_slots.live_count) as usize
    }

    pub fn node_count(&self) -> usize {
        self.spatial_nodes.len() + self.clip_nodes.len() + self.effect_nodes.len()
    }

    pub fn dead_node_count(&self) -> usize {
        self.node_count() - self.live_node_count()
    }

    pub fn should_compact(&self) -> bool {
        self.dead_node_count() > self.live_node_count().max(COMPACTION_DEAD_NODE_THRESHOLD)
    }

    pub fn allocate_spatial_slot(&mut self) -> (SpatialNodeIndex, bool) {
        allocate_slot(&mut self.spatial_nodes, &mut self.spatial_slots)
    }

    pub fn allocate_clip_slot(&mut self) -> (ClipNodeIndex, bool) {
        allocate_slot(&mut self.clip_nodes, &mut self.clip_slots)
    }

    pub fn allocate_effect_slot(&mut self) -> (EffectNodeIndex, bool) {
        allocate_slot(&mut self.effect_nodes, &mut self.effect_slots)
    }

    pub fn tombstone_spatial_slot(&mut self, index: SpatialNodeIndex) -> bool {
        assert_ne!(
            index, VISUAL_VIEWPORT_NODE_INDEX,
            "the visual viewport node is never tombstoned"
        );
        tombstone_slot(&mut self.spatial_nodes, &mut self.spatial_slots, index)
    }

    pub fn tombstone_clip_slot(&mut self, index: ClipNodeIndex) -> bool {
        tombstone_slot(&mut self.clip_nodes, &mut self.clip_slots, index)
    }

    pub fn tombstone_effect_slot(&mut self, index: EffectNodeIndex) -> bool {
        assert_ne!(
            Some(index),
            self.root_isolation_effect,
            "the root isolation effect is never tombstoned"
        );
        tombstone_slot(&mut self.effect_nodes, &mut self.effect_slots, index)
    }

    pub fn replace_spatial_node(&mut self, index: SpatialNodeIndex, node: SpatialNode) -> bool {
        replace_slot(&mut self.spatial_nodes, &mut self.spatial_slots, index, node)
    }

    pub fn replace_clip_node(&mut self, index: ClipNodeIndex, node: ClipNode) -> bool {
        replace_slot(&mut self.clip_nodes, &mut self.clip_slots, index, node)
    }

    pub fn replace_effect_node(&mut self, index: EffectNodeIndex, node: EffectNode) -> bool {
        replace_slot(&mut self.effect_nodes, &mut self.effect_slots, index, node)
    }

    pub fn release_quarantined_slots_after_recording(&mut self) {
        self.spatial_slots.release_quarantined();
        self.clip_slots.release_quarantined();
        self.effect_slots.release_quarantined();
        self.debug_assert_slot_accounting();
    }

    pub fn free_slot_count(&self) -> usize {
        self.spatial_slots.free.len() + self.clip_slots.free.len() + self.effect_slots.free.len()
    }

    pub fn quarantined_slot_count(&self) -> usize {
        self.spatial_slots.quarantined.len() + self.clip_slots.quarantined.len() + self.effect_slots.quarantined.len()
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
        self.spatial_slots.live_count += 1;
        SpatialNodeIndex((self.spatial_nodes.len() - 1) as u32)
    }

    pub fn append_spatial_under(&mut self, context: ContextRef, data: SpatialData) -> ContextRef {
        ContextRef {
            spatial: self.append_spatial(data, context.spatial),
            ..context
        }
    }

    pub fn append_clip(
        &mut self,
        data: ClipNodeData,
        parent: ClipNodeIndex,
        spatial: SpatialNodeIndex,
    ) -> ClipNodeIndex {
        assert!(
            self.spatial_is_live(spatial),
            "a clip node's spatial node must be a live node"
        );
        assert!(data.is_live(), "appended clip nodes are live");
        if !parent.is_none() {
            assert!(self.clip_is_live(parent), "a clip node's parent must be a live node");
            let parent_node = &self.clip_nodes[parent.0 as usize];
            debug_assert!(self.spatial_is_ancestor_or_self(parent_node.spatial, spatial));
        }
        self.clip_nodes.push(ClipNode::new(data, parent, spatial));
        self.clip_slots.live_count += 1;
        ClipNodeIndex((self.clip_nodes.len() - 1) as u32)
    }

    pub fn append_effect(
        &mut self,
        data: EffectNodeData,
        parent: EffectNodeIndex,
        spatial: SpatialNodeIndex,
        output_clip: ClipNodeIndex,
    ) -> EffectNodeIndex {
        assert!(
            self.spatial_is_live(spatial),
            "an effect node's spatial node must be a live node"
        );
        assert!(data.is_live(), "appended effect nodes are live");
        assert!(
            self.clip_is_none_or_live(output_clip),
            "an effect node's output clip must be a live node"
        );
        if !parent.is_none() {
            assert!(
                self.effect_is_live(parent),
                "an effect node's parent must be a live node"
            );
        }
        self.effect_nodes
            .push(EffectNode::new(data, parent, spatial, Some(output_clip)));
        self.effect_slots.live_count += 1;
        EffectNodeIndex((self.effect_nodes.len() - 1) as u32)
    }

    pub fn clip_is_ancestor_or_self(&self, ancestor: ClipNodeIndex, node: ClipNodeIndex) -> bool {
        VisualContextNodeSink::clip_is_ancestor_or_self(self, ancestor, node)
    }

    // An effect only this chain records under already has its final output clip.
    pub fn append_effect_node_under(&mut self, context: ContextRef, data: EffectNodeData) -> ContextRef {
        let effect = self.append_effect(data, context.effect, context.spatial, context.clip);
        ContextRef { effect, ..context }
    }

    // Resolve layer placement from the clips where effects begin and from positioned
    // descendants that escape clips. These constraints belong to the boxes that build them;
    // they have no identity or lifetime in the published tree. Report changes to previously
    // resolved clips; resolving a newly allocated effect does not invalidate existing handles.
    pub fn resolve_effect_output_clips(&mut self, constraints: &[EffectClipConstraint]) -> bool {
        let clip_depths = self.clip_depths();
        let depth_of = |index: ClipNodeIndex| {
            if index.is_none() {
                0
            } else {
                clip_depths[index.0 as usize]
            }
        };
        let parent_of = |index: ClipNodeIndex| self.clip_nodes[index.0 as usize].parent;
        let mut shared: Vec<Option<ClipNodeIndex>> = vec![None; self.effect_nodes.len()];
        let narrow = |shared: &mut Option<ClipNodeIndex>, clip: ClipNodeIndex| {
            *shared = Some(match *shared {
                Some(current) => clip_lowest_common_ancestor_with_depths(parent_of, depth_of, current, clip),
                None => clip,
            });
        };
        if let Some(effect) = self.root_isolation_effect {
            narrow(&mut shared[effect.0 as usize], ClipNodeIndex::NONE);
        }
        for constraint in constraints {
            if !constraint.effect.is_none() {
                narrow(&mut shared[constraint.effect.0 as usize], constraint.clip);
            }
        }
        // Parents come first in the dependency order, so walking it backwards folds every
        // effect's clip into its parent's before the parent is read.
        for &index in self.effect_dependency_order().iter().rev() {
            let parent = self.effect_nodes[index as usize].parent;
            if parent.is_none() {
                continue;
            }
            if let Some(clip) = shared[index as usize] {
                narrow(&mut shared[parent.0 as usize], clip);
            }
        }
        let mut changed = false;
        for (node, shared) in self.effect_nodes.iter_mut().zip(&shared) {
            if let Some(clip) = *shared {
                changed |= node.resolved_output_clip.is_some_and(|previous| previous != clip);
                node.resolved_output_clip = Some(clip);
            }
        }
        changed
    }

    // Root path lengths of the clip nodes; the absent clip has depth 0.
    pub fn clip_depths(&self) -> Vec<u32> {
        let mut depths = vec![0; self.clip_nodes.len()];
        for index in self.clip_dependency_order() {
            let parent = self.clip_nodes[index as usize].parent;
            depths[index as usize] = if parent.is_none() {
                1
            } else {
                depths[parent.0 as usize] + 1
            };
        }
        depths
    }

    pub fn spatial_is_ancestor_or_self(&self, ancestor: SpatialNodeIndex, mut node: SpatialNodeIndex) -> bool {
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

    pub(crate) fn clip_dependency_order_with_back_edges(&self) -> NodeDependencyOrder {
        dependency_order(
            self.clip_nodes.len(),
            |index| self.clip_nodes[index].data.is_live(),
            |index, references| {
                let parent = self.clip_nodes[index].parent;
                if !parent.is_none() {
                    references.push(parent.0 as usize);
                }
            },
        )
    }

    // Output clips live in the clip tree, so they are validated separately rather than ordered here.
    pub(crate) fn effect_dependency_order_with_back_edges(&self) -> NodeDependencyOrder {
        dependency_order(
            self.effect_nodes.len(),
            |index| self.effect_nodes[index].data.is_live(),
            |index, references| {
                let parent = self.effect_nodes[index].parent;
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

    pub fn clip_dependency_order(&self) -> Vec<u32> {
        let dependency_order = self.clip_dependency_order_with_back_edges();
        debug_assert!(
            dependency_order.back_edges.is_empty() && dependency_order.dangling_references.is_empty(),
            "clip node parents form a cycle or dangle: {:?} {:?}",
            dependency_order.back_edges,
            dependency_order.dangling_references
        );
        dependency_order.order
    }

    pub fn effect_dependency_order(&self) -> Vec<u32> {
        let dependency_order = self.effect_dependency_order_with_back_edges();
        debug_assert!(
            dependency_order.back_edges.is_empty() && dependency_order.dangling_references.is_empty(),
            "effect node parents form a cycle or dangle: {:?} {:?}",
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

pub fn clip_is_ancestor_or_self(
    parent_of: impl Fn(ClipNodeIndex) -> ClipNodeIndex,
    ancestor: ClipNodeIndex,
    mut node: ClipNodeIndex,
) -> bool {
    loop {
        if node == ancestor {
            return true;
        }
        if node.is_none() {
            return false;
        }
        node = parent_of(node);
    }
}

// The deepest clip on both root paths; the absent clip when the paths only share the root. The
// deeper node climbs first, then both climb together.
pub fn clip_lowest_common_ancestor_with_depths(
    parent_of: impl Fn(ClipNodeIndex) -> ClipNodeIndex,
    depth_of: impl Fn(ClipNodeIndex) -> u32,
    mut a: ClipNodeIndex,
    mut b: ClipNodeIndex,
) -> ClipNodeIndex {
    if a == b {
        return a;
    }
    let mut a_depth = depth_of(a);
    let mut b_depth = depth_of(b);
    while a_depth > b_depth {
        a = parent_of(a);
        a_depth -= 1;
    }
    while b_depth > a_depth {
        b = parent_of(b);
        b_depth -= 1;
    }
    while a != b {
        a = parent_of(a);
        b = parent_of(b);
    }
    a
}

pub trait VisualContextNodeSink {
    fn append_spatial_node(&mut self, data: SpatialData, parent: SpatialNodeIndex) -> SpatialNodeIndex;
    fn append_clip_node(
        &mut self,
        data: ClipNodeData,
        parent: ClipNodeIndex,
        spatial: SpatialNodeIndex,
    ) -> ClipNodeIndex;
    fn spatial_node_at(&self, index: SpatialNodeIndex) -> &SpatialNode;
    fn clip_node_at(&self, index: ClipNodeIndex) -> &ClipNode;

    fn append_spatial_node_under(&mut self, context: ContextRef, data: SpatialData) -> ContextRef {
        ContextRef {
            spatial: self.append_spatial_node(data, context.spatial),
            ..context
        }
    }

    fn append_clip_node_under(&mut self, context: ContextRef, data: ClipNodeData) -> ContextRef {
        let clip = self.append_clip_node(data, context.clip, context.spatial);
        ContextRef { clip, ..context }
    }

    fn clip_is_ancestor_or_self(&self, ancestor: ClipNodeIndex, node: ClipNodeIndex) -> bool {
        clip_is_ancestor_or_self(|index| self.clip_node_at(index).parent, ancestor, node)
    }
}

impl VisualContextNodeSink for VisualContextTree {
    fn append_spatial_node(&mut self, data: SpatialData, parent: SpatialNodeIndex) -> SpatialNodeIndex {
        self.append_spatial(data, parent)
    }

    fn append_clip_node(
        &mut self,
        data: ClipNodeData,
        parent: ClipNodeIndex,
        spatial: SpatialNodeIndex,
    ) -> ClipNodeIndex {
        self.append_clip(data, parent, spatial)
    }

    fn spatial_node_at(&self, index: SpatialNodeIndex) -> &SpatialNode {
        &self.spatial_nodes[index.0 as usize]
    }

    fn clip_node_at(&self, index: ClipNodeIndex) -> &ClipNode {
        &self.clip_nodes[index.0 as usize]
    }
}

// The nodes a box appended, by kind. The chain units end at the box's own accumulated
// context; the descendant units hold what only descendants record under (the overflow clip).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct BoxVisualContextNodeHandles {
    pub spatial: Vec<SpatialNodeIndex>,
    pub chain_clips: Vec<ClipNodeIndex>,
    pub descendant_clips: Vec<ClipNodeIndex>,
    pub effects: Vec<EffectNodeIndex>,
}

pub static EMPTY_BOX_VISUAL_CONTEXT_NODE_HANDLES: BoxVisualContextNodeHandles = BoxVisualContextNodeHandles {
    spatial: Vec::new(),
    chain_clips: Vec::new(),
    descendant_clips: Vec::new(),
    effects: Vec::new(),
};

impl BoxVisualContextNodeHandles {
    pub fn clip_handles(&self) -> impl Iterator<Item = ClipNodeIndex> + '_ {
        self.chain_clips.iter().chain(&self.descendant_clips).copied()
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

// One positioning chain; effects are shared by all three chains.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct PositioningContext {
    pub spatial: SpatialNodeIndex,
    pub clip: ClipNodeIndex,
    pub nearest_scroll_nodes: NearestScrollNodeIndices,
    pub plane_root: SpatialNodeIndex,
}

impl PositioningContext {
    pub fn with_effect(self, effect: EffectNodeIndex) -> ContextRef {
        ContextRef {
            spatial: self.spatial,
            clip: self.clip,
            effect,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EffectClipConstraint {
    pub effect: EffectNodeIndex,
    pub clip: ClipNodeIndex,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct DescendantVisualContexts {
    pub effect: EffectNodeIndex,
    pub normal: PositioningContext,
    pub absolute_position: PositioningContext,
    pub fixed_position: PositioningContext,
    pub flattens_inherited_transform: bool,
    pub sorting_context_root: Option<SpatialNodeIndex>,
    pub enclosing_stacking_context: NodeSlotId,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct PaintableVisualContextRecord {
    pub inherited_input: DescendantVisualContexts,
    pub output_for_descendants: DescendantVisualContexts,
    pub node_handles: BoxVisualContextNodeHandles,
    pub effect_clip_constraints: Vec<EffectClipConstraint>,
    pub has_mask_nodes: bool,
    pub may_be_root_element: bool,
    pub owns_geometry_dependent_nodes: bool,
    pub subtree_may_own_geometry_dependent_nodes: bool,
    pub stacking_context: crate::painting::stacking_context::StackingContextFacts,
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

    fn effects() -> EffectNodeData {
        EffectNodeData::Effects(EffectsData {
            opacity: 1.0,
            blend_mode: CompositingAndBlendingOperator::Normal,
            filter: None,
        })
    }

    fn clip(rect: FloatRect, mode: ClipMode) -> ClipNodeData {
        ClipNodeData::Rect(ClipData {
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
    fn clip_and_effect_dependency_orders_follow_parents_only() {
        let mut tree = tree();
        let root_effect = tree.append_effect(
            effects(),
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );
        let child = tree.append_effect(effects(), root_effect, VISUAL_VIEWPORT_NODE_INDEX, ClipNodeIndex::NONE);
        let parent = tree.append_effect(effects(), root_effect, VISUAL_VIEWPORT_NODE_INDEX, ClipNodeIndex::NONE);
        tree.effect_nodes[child.0 as usize].parent = parent;
        assert_eq!(tree.effect_dependency_order(), vec![root_effect.0, parent.0, child.0]);

        let outer = tree.append_clip(
            clip(FloatRect::new(0.0, 0.0, 1.0, 1.0), ClipMode::Intersect),
            ClipNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let inner_child = tree.append_clip(
            clip(FloatRect::new(0.0, 0.0, 1.0, 1.0), ClipMode::Intersect),
            outer,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let inner = tree.append_clip(
            clip(FloatRect::new(0.0, 0.0, 1.0, 1.0), ClipMode::Intersect),
            outer,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        tree.clip_nodes[inner_child.0 as usize].parent = inner;
        assert_eq!(tree.clip_dependency_order(), vec![outer.0, inner.0, inner_child.0]);
    }

    #[test]
    fn the_lowest_common_clip_ancestor_is_the_deepest_shared_clip() {
        let mut tree = tree();
        let rect = || clip(FloatRect::new(0.0, 0.0, 1.0, 1.0), ClipMode::Intersect);
        let root = tree.append_clip(rect(), ClipNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX);
        let left = tree.append_clip(rect(), root, VISUAL_VIEWPORT_NODE_INDEX);
        let left_leaf = tree.append_clip(rect(), left, VISUAL_VIEWPORT_NODE_INDEX);
        let right = tree.append_clip(rect(), root, VISUAL_VIEWPORT_NODE_INDEX);
        let other_root = tree.append_clip(rect(), ClipNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX);
        let depths = tree.clip_depths();
        let common_ancestor = |a, b| {
            clip_lowest_common_ancestor_with_depths(
                |index| tree.clip_nodes[index.0 as usize].parent,
                |index| if index.is_none() { 0 } else { depths[index.0 as usize] },
                a,
                b,
            )
        };
        assert_eq!(common_ancestor(left_leaf, right), root);
        assert_eq!(common_ancestor(left_leaf, left), left);
        assert_eq!(common_ancestor(left, left_leaf), left);
        assert_eq!(common_ancestor(left_leaf, left_leaf), left_leaf);
        assert_eq!(common_ancestor(left_leaf, other_root), ClipNodeIndex::NONE);
        assert_eq!(common_ancestor(ClipNodeIndex::NONE, right), ClipNodeIndex::NONE);
        assert!(tree.clip_is_ancestor_or_self(ClipNodeIndex::NONE, left_leaf));
        assert!(tree.clip_is_ancestor_or_self(root, left_leaf));
        assert!(!tree.clip_is_ancestor_or_self(right, left_leaf));
    }

    #[test]
    fn contexts_name_spatial_clip_and_effect_nodes_directly() {
        let mut tree = tree();
        let root = ContextRef::spatial_only(VISUAL_VIEWPORT_NODE_INDEX);
        let clipped = tree.append_clip_node_under(root, clip(FloatRect::new(0.0, 0.0, 1.0, 1.0), ClipMode::Intersect));
        let layered = tree.append_effect_node_under(clipped, effects());
        assert_eq!(layered.spatial, root.spatial);
        assert_eq!(layered.clip, clipped.clip);
        assert_eq!(clipped.effect, EffectNodeIndex::NONE);
        assert_eq!(tree.effect_nodes[layered.effect.0 as usize].output_clip(), clipped.clip);
        assert_eq!(tree.live_node_count(), 3);
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
    fn effect_clip_constraints_include_positioned_escapes_and_descendant_effects() {
        let mut tree = tree();
        let rect = || clip(FloatRect::new(0.0, 0.0, 1.0, 1.0), ClipMode::Intersect);
        let outer = tree.append_clip(rect(), ClipNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX);
        let inner = tree.append_clip(rect(), outer, VISUAL_VIEWPORT_NODE_INDEX);
        let sibling = tree.append_clip(rect(), outer, VISUAL_VIEWPORT_NODE_INDEX);
        let layer = tree.append_effect(
            effects(),
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );
        let nested = tree.append_effect(effects(), layer, VISUAL_VIEWPORT_NODE_INDEX, ClipNodeIndex::NONE);
        let mut constraints = vec![
            EffectClipConstraint {
                effect: layer,
                clip: inner,
            },
            EffectClipConstraint {
                effect: nested,
                clip: inner,
            },
        ];
        assert!(tree.resolve_effect_output_clips(&constraints));
        assert_eq!(tree.effect_nodes[layer.0 as usize].output_clip(), inner);
        assert_eq!(tree.effect_nodes[nested.0 as usize].output_clip(), inner);
        assert!(!tree.resolve_effect_output_clips(&constraints));
        constraints.push(EffectClipConstraint {
            effect: nested,
            clip: sibling,
        });
        assert!(tree.resolve_effect_output_clips(&constraints));
        assert_eq!(tree.effect_nodes[nested.0 as usize].output_clip(), outer);
        assert_eq!(tree.effect_nodes[layer.0 as usize].output_clip(), outer);
        constraints.pop();
        assert!(tree.resolve_effect_output_clips(&constraints));
        assert_eq!(tree.effect_nodes[layer.0 as usize].output_clip(), inner);
        assert!(!tree.resolve_effect_output_clips(&constraints));
    }

    #[test]
    fn tombstones_keep_their_links_and_leave_the_live_counts() {
        let mut tree = tree();
        let transform = tree.append_spatial(SpatialData::Transform(transform(0.0)), VISUAL_VIEWPORT_NODE_INDEX);
        let empty_clip = tree.append_clip(
            clip(FloatRect::new(0.0, 0.0, 0.0, 0.0), ClipMode::Intersect),
            ClipNodeIndex::NONE,
            transform,
        );
        let effect = tree.append_effect(effects(), EffectNodeIndex::NONE, transform, empty_clip);
        assert_eq!(tree.live_spatial_node_count(), 2);
        assert_eq!(tree.live_clip_node_count(), 1);
        assert_eq!(tree.live_effect_node_count(), 1);
        assert!(tree.clip_nodes[empty_clip.0 as usize].clips_everything);
        assert!(tree.tombstone_effect_slot(effect));
        assert!(tree.tombstone_clip_slot(empty_clip));
        assert!(tree.tombstone_spatial_slot(transform));
        assert!(!tree.tombstone_spatial_slot(transform));
        assert!(!tree.tombstone_clip_slot(empty_clip));
        assert!(!tree.spatial_is_live(transform));
        assert!(!tree.clip_is_live(empty_clip));
        assert!(!tree.effect_is_live(effect));
        assert_eq!(
            tree.spatial_nodes[transform.0 as usize].parent,
            VISUAL_VIEWPORT_NODE_INDEX
        );
        assert_eq!(tree.clip_nodes[empty_clip.0 as usize].parent, ClipNodeIndex::NONE);
        assert!(!tree.clip_nodes[empty_clip.0 as usize].clips_everything);
        assert_eq!(tree.effect_nodes[effect.0 as usize].output_clip(), empty_clip);
        assert_eq!(tree.live_spatial_node_count(), 1);
        assert_eq!(tree.live_clip_node_count(), 0);
        assert_eq!(tree.live_effect_node_count(), 0);
        assert_eq!(tree.dead_node_count(), 3);
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
        let first = tree.append_effect(
            effects(),
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );
        let second = tree.append_effect(
            effects(),
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );

        assert!(tree.tombstone_effect_slot(first));
        assert!(tree.tombstone_effect_slot(second));
        tree.release_quarantined_slots_after_recording();
        let effect_count = tree.effect_nodes.len();
        assert_eq!(tree.allocate_effect_slot(), (second, true));
        assert_eq!(tree.allocate_effect_slot(), (first, true));
        assert_eq!(tree.effect_nodes.len(), effect_count);
        assert_eq!(
            tree.allocate_effect_slot(),
            (EffectNodeIndex(effect_count as u32), false)
        );
    }

    #[test]
    fn dead_node_count_counts_free_and_quarantined_slots() {
        let mut tree = tree();
        let transform_node = tree.append_spatial(SpatialData::Transform(transform(0.0)), VISUAL_VIEWPORT_NODE_INDEX);
        let clip_node = tree.append_clip(
            clip(FloatRect::new(0.0, 0.0, 1.0, 1.0), ClipMode::Intersect),
            ClipNodeIndex::NONE,
            transform_node,
        );

        assert!(tree.tombstone_clip_slot(clip_node));
        tree.release_quarantined_slots_after_recording();
        assert!(tree.tombstone_spatial_slot(transform_node));
        assert_eq!(tree.dead_node_count(), 2);
        assert_eq!(tree.free_slot_count(), 1);
        assert_eq!(tree.quarantined_slot_count(), 1);
        let (slot, reused) = tree.allocate_clip_slot();
        assert!(reused);
        assert!(!tree.replace_clip_node(
            slot,
            ClipNode::new(
                clip(FloatRect::new(0.0, 0.0, 1.0, 1.0), ClipMode::Intersect),
                ClipNodeIndex::NONE,
                VISUAL_VIEWPORT_NODE_INDEX
            )
        ));
        assert_eq!(tree.dead_node_count(), 1);
        tree.debug_assert_slot_accounting();
    }

    #[test]
    fn dependency_orders_leave_tombstones_out() {
        let mut tree = tree();
        let stale = tree.append_spatial(SpatialData::Transform(transform(0.0)), VISUAL_VIEWPORT_NODE_INDEX);
        let other = tree.append_spatial(SpatialData::Transform(transform(0.0)), VISUAL_VIEWPORT_NODE_INDEX);
        let effect = tree.append_effect(effects(), EffectNodeIndex::NONE, other, ClipNodeIndex::NONE);
        let clip_node = tree.append_clip(
            clip(FloatRect::new(0.0, 0.0, 1.0, 1.0), ClipMode::Intersect),
            ClipNodeIndex::NONE,
            other,
        );
        assert!(tree.tombstone_spatial_slot(stale));
        assert!(tree.tombstone_effect_slot(effect));
        assert!(tree.tombstone_clip_slot(clip_node));
        let order = tree.spatial_dependency_order_with_back_edges();
        assert_eq!(order.order, vec![VISUAL_VIEWPORT_NODE_INDEX.0, other.0]);
        assert!(order.back_edges.is_empty());
        assert!(order.dangling_references.is_empty());
        assert!(tree.effect_dependency_order().is_empty());
        assert!(tree.clip_dependency_order().is_empty());
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
