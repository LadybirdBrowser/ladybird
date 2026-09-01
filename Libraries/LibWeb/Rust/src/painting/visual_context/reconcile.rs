/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::delta::VisualContextTreeDelta;
use super::shape::{frame_node_shape, frame_payloads_are_equal, spatial_node_shape, spatial_payloads_are_equal};
use super::*;

pub(crate) struct BoxNodeScratch<'a> {
    live: &'a VisualContextTree,
    provisional_spatial_begin: u32,
    provisional_frames_begin: u32,
    spatial: Vec<SpatialNode>,
    frames: Vec<FrameNode>,
}

impl<'a> BoxNodeScratch<'a> {
    pub(crate) fn new(live: &'a VisualContextTree) -> Self {
        Self {
            live,
            provisional_spatial_begin: live.spatial_nodes.len() as u32,
            provisional_frames_begin: live.frame_nodes.len() as u32,
            spatial: Vec::new(),
            frames: Vec::new(),
        }
    }

    pub(crate) fn into_nodes(self) -> (Vec<SpatialNode>, Vec<FrameNode>) {
        (self.spatial, self.frames)
    }

    fn frame_node_at(&self, index: FrameNodeIndex) -> &FrameNode {
        if index.0 >= self.provisional_frames_begin {
            &self.frames[(index.0 - self.provisional_frames_begin) as usize]
        } else {
            &self.live.frame_nodes[index.0 as usize]
        }
    }
}

impl VisualContextNodeSink for BoxNodeScratch<'_> {
    fn append_spatial_node(&mut self, data: SpatialData, parent: SpatialNodeIndex) -> SpatialNodeIndex {
        debug_assert!(self.spatial_node_at(parent).data.is_live());
        self.spatial.push(SpatialNode { data, parent });
        SpatialNodeIndex(self.provisional_spatial_begin + self.spatial.len() as u32 - 1)
    }

    fn append_frame_node(
        &mut self,
        data: FrameData,
        parent: FrameNodeIndex,
        spatial: SpatialNodeIndex,
    ) -> FrameNodeIndex {
        debug_assert!(self.spatial_node_at(spatial).data.is_live());
        debug_assert!(parent.is_none() || self.frame_node_at(parent).data.is_live());
        self.frames.push(FrameNode::new(data, parent, spatial));
        FrameNodeIndex(self.provisional_frames_begin + self.frames.len() as u32 - 1)
    }

    fn spatial_node_at(&self, index: SpatialNodeIndex) -> &SpatialNode {
        if index.0 >= self.provisional_spatial_begin {
            &self.spatial[(index.0 - self.provisional_spatial_begin) as usize]
        } else {
            &self.live.spatial_nodes[index.0 as usize]
        }
    }

    fn next_spatial_node_index(&self) -> SpatialNodeIndex {
        SpatialNodeIndex(self.provisional_spatial_begin + self.spatial.len() as u32)
    }

    fn next_frame_node_index(&self) -> FrameNodeIndex {
        FrameNodeIndex(self.provisional_frames_begin + self.frames.len() as u32)
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct BoxNodePlacement {
    provisional_spatial_begin: u32,
    provisional_frames_begin: u32,
    pub spatial: Vec<SpatialNodeIndex>,
    pub chain_frames: Vec<FrameNodeIndex>,
    pub descendant_frames: Vec<FrameNodeIndex>,
}

impl BoxNodePlacement {
    pub(crate) fn remap_spatial(&self, index: SpatialNodeIndex) -> SpatialNodeIndex {
        if index.0 >= self.provisional_spatial_begin {
            self.spatial[(index.0 - self.provisional_spatial_begin) as usize]
        } else {
            index
        }
    }

    pub(crate) fn remap_frame(&self, index: FrameNodeIndex) -> FrameNodeIndex {
        if index.is_none() || index.0 < self.provisional_frames_begin {
            return index;
        }
        let mut offset = (index.0 - self.provisional_frames_begin) as usize;
        for unit in [&self.chain_frames, &self.descendant_frames] {
            if offset < unit.len() {
                return unit[offset];
            }
            offset -= unit.len();
        }
        panic!("provisional frame {} lies outside the box's units", index.0)
    }

    pub(crate) fn remap_context(&self, context: ContextRef) -> ContextRef {
        ContextRef {
            spatial: self.remap_spatial(context.spatial),
            frame: self.remap_frame(context.frame),
        }
    }

    pub(crate) fn into_node_handles(self) -> BoxVisualContextNodeHandles {
        BoxVisualContextNodeHandles {
            spatial: self.spatial,
            chain_frames: self.chain_frames,
            descendant_frames: self.descendant_frames,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct ReconcileOutcome {
    pub shape_changed: bool,
    pub any_payload_changed: bool,
}

fn provisional_begin<Handle: Copy>(handles: &[Handle], index_of: impl Fn(Handle) -> u32) -> u32 {
    handles.first().map_or(u32::MAX, |first| index_of(*first))
}

fn place_unit<Handle: Copy>(
    existing: &[Handle],
    new_len: usize,
    mut allocate: impl FnMut() -> (Handle, bool),
    mut note_allocated: impl FnMut(Handle, bool),
) -> Vec<Handle> {
    let mut handles: Vec<Handle> = existing.iter().take(new_len).copied().collect();
    while handles.len() < new_len {
        let (handle, reused) = allocate();
        note_allocated(handle, reused);
        handles.push(handle);
    }
    handles
}

pub(crate) fn plan_box_node_placement(
    tree: &mut VisualContextTree,
    existing: Option<&BoxVisualContextNodeHandles>,
    provisional: &BoxVisualContextNodeHandles,
    delta: &mut VisualContextTreeDelta,
) -> BoxNodePlacement {
    let existing = existing.unwrap_or(&EMPTY_BOX_VISUAL_CONTEXT_NODE_HANDLES);
    let provisional_frames: Vec<FrameNodeIndex> = provisional.frame_handles().collect();
    debug_assert!(
        provisional.spatial.windows(2).all(|pair| pair[1].0 == pair[0].0 + 1)
            && provisional_frames.windows(2).all(|pair| pair[1].0 == pair[0].0 + 1),
        "a scratch build hands out consecutive provisional indices"
    );
    let spatial = place_unit(
        &existing.spatial,
        provisional.spatial.len(),
        || tree.allocate_spatial_slot(),
        |handle, reused| delta.note_allocated_spatial(handle.0, reused),
    );
    let mut place_frames = |existing: &[FrameNodeIndex], new_len: usize| {
        place_unit(
            existing,
            new_len,
            || tree.allocate_frame_slot(),
            |handle, reused| delta.note_allocated_frame(handle.0, reused),
        )
    };
    let chain_frames = place_frames(&existing.chain_frames, provisional.chain_frames.len());
    let descendant_frames = place_frames(&existing.descendant_frames, provisional.descendant_frames.len());
    BoxNodePlacement {
        provisional_spatial_begin: provisional_begin(&provisional.spatial, |index| index.0),
        provisional_frames_begin: provisional_begin(&provisional_frames, |index| index.0),
        spatial,
        chain_frames,
        descendant_frames,
    }
}

fn remap_spatial_payload(placement: &BoxNodePlacement, data: SpatialData) -> SpatialData {
    match data {
        SpatialData::Scroll(mut scroll) => {
            scroll.registry_parent_node = placement.remap_spatial(scroll.registry_parent_node);
            SpatialData::Scroll(scroll)
        }
        SpatialData::Sticky(mut sticky) => {
            sticky.scroller = placement.remap_spatial(sticky.scroller);
            sticky.parent_sticky = sticky.parent_sticky.map(|index| placement.remap_spatial(index));
            sticky.registry_parent_node = placement.remap_spatial(sticky.registry_parent_node);
            SpatialData::Sticky(sticky)
        }
        SpatialData::Transform(mut transform) => {
            transform.sorting_context_root_index = transform
                .sorting_context_root_index
                .map(|index| placement.remap_spatial(index));
            SpatialData::Transform(transform)
        }
        SpatialData::BackfaceVisibility(mut backface) => {
            backface.plane_root_index = placement.remap_spatial(backface.plane_root_index);
            SpatialData::BackfaceVisibility(backface)
        }
        SpatialData::AnchorScrollShift(mut shift) => {
            shift.scroll_node_index = placement.remap_spatial(shift.scroll_node_index);
            SpatialData::AnchorScrollShift(shift)
        }
        other => other,
    }
}

fn write_spatial_unit(
    tree: &mut VisualContextTree,
    handles: &[SpatialNodeIndex],
    existing: &[SpatialNodeIndex],
    nodes: Vec<SpatialNode>,
    delta: &mut VisualContextTreeDelta,
    outcome: &mut ReconcileOutcome,
) {
    debug_assert_eq!(handles.len(), nodes.len());
    if handles.len() != existing.len() {
        outcome.shape_changed = true;
    }
    for (handle, node) in handles.iter().zip(nodes) {
        let current = &tree.spatial_nodes[handle.0 as usize];
        let was_live = current.data.is_live();
        let shape_matches = was_live && spatial_node_shape(current) == spatial_node_shape(&node);
        let payload_matches = shape_matches && spatial_payloads_are_equal(&current.data, &node.data);
        if !shape_matches {
            outcome.shape_changed = true;
            if was_live {
                delta.note_repurposed_in_place();
            }
        }
        if payload_matches {
            continue;
        }
        outcome.any_payload_changed = true;
        tree.replace_spatial_node(*handle, node);
        delta.note_patched_spatial(handle.0);
    }
    for surplus in existing.get(handles.len()..).unwrap_or(&[]) {
        if tree.tombstone_spatial_slot(*surplus) {
            delta.note_tombstoned_spatial(surplus.0);
        }
    }
}

fn write_frame_unit(
    tree: &mut VisualContextTree,
    handles: &[FrameNodeIndex],
    existing: &[FrameNodeIndex],
    nodes: Vec<FrameNode>,
    delta: &mut VisualContextTreeDelta,
    outcome: &mut ReconcileOutcome,
) {
    debug_assert_eq!(handles.len(), nodes.len());
    if handles.len() != existing.len() {
        outcome.shape_changed = true;
    }
    for (handle, node) in handles.iter().zip(nodes) {
        let current = &tree.frame_nodes[handle.0 as usize];
        let was_live = current.data.is_live();
        let shape_matches = was_live && frame_node_shape(current) == frame_node_shape(&node);
        let payload_matches = shape_matches && frame_payloads_are_equal(&current.data, &node.data);
        if !shape_matches {
            outcome.shape_changed = true;
            if was_live {
                delta.note_repurposed_in_place();
            }
        }
        if payload_matches {
            continue;
        }
        outcome.any_payload_changed = true;
        if matches!(node.data, FrameData::Mask(_)) {
            delta.requires_display_list_recording = true;
        }
        tree.replace_frame_node(*handle, node);
        delta.note_patched_frame(handle.0);
    }
    for surplus in existing.get(handles.len()..).unwrap_or(&[]) {
        if tree.tombstone_frame_slot(*surplus) {
            delta.note_tombstoned_frame(surplus.0);
        }
    }
}

pub(crate) fn write_box_nodes(
    tree: &mut VisualContextTree,
    spatial: Vec<SpatialNode>,
    frames: Vec<FrameNode>,
    placement: &BoxNodePlacement,
    existing: Option<&BoxVisualContextNodeHandles>,
    delta: &mut VisualContextTreeDelta,
) -> ReconcileOutcome {
    let existing = existing.unwrap_or(&EMPTY_BOX_VISUAL_CONTEXT_NODE_HANDLES);
    let mut outcome = ReconcileOutcome::default();
    let spatial_nodes: Vec<SpatialNode> = spatial
        .into_iter()
        .map(|node| SpatialNode {
            data: remap_spatial_payload(placement, node.data),
            parent: placement.remap_spatial(node.parent),
        })
        .collect();
    let mut frames = frames.into_iter().map(|node| {
        FrameNode::new(
            node.data,
            placement.remap_frame(node.parent),
            placement.remap_spatial(node.spatial),
        )
    });
    let chain: Vec<FrameNode> = frames.by_ref().take(placement.chain_frames.len()).collect();
    let descendant: Vec<FrameNode> = frames.collect();
    debug_assert_eq!(descendant.len(), placement.descendant_frames.len());

    write_spatial_unit(
        tree,
        &placement.spatial,
        &existing.spatial,
        spatial_nodes,
        delta,
        &mut outcome,
    );
    write_frame_unit(
        tree,
        &placement.chain_frames,
        &existing.chain_frames,
        chain,
        delta,
        &mut outcome,
    );
    write_frame_unit(
        tree,
        &placement.descendant_frames,
        &existing.descendant_frames,
        descendant,
        delta,
        &mut outcome,
    );
    outcome
}

#[cfg(test)]
mod tests {
    use super::*;
    use libgfx_rust::FloatMatrix4x4;

    fn transform_data() -> TransformData {
        TransformData {
            matrix: FloatMatrix4x4::identity(),
            origin: FloatPoint::default(),
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
        }
    }

    fn transform() -> SpatialData {
        SpatialData::Transform(transform_data())
    }

    fn clip(size: f32) -> FrameData {
        FrameData::rect_clip(FloatRect::new(0.0, 0.0, size, size))
    }

    const VIEWPORT_SCROLL: SpatialNodeIndex = SpatialNodeIndex(1);
    const ROOT_ISOLATION_FRAME: FrameNodeIndex = FrameNodeIndex(0);
    const BOX_A_SPATIAL: SpatialNodeIndex = SpatialNodeIndex(2);
    const BOX_B_SPATIAL: SpatialNodeIndex = SpatialNodeIndex(3);
    const BOX_A_CHAIN_FRAME: FrameNodeIndex = FrameNodeIndex(1);
    const BOX_A_PATCHED_CHAIN_FRAME: FrameNodeIndex = FrameNodeIndex(2);
    const BOX_A_DESCENDANT_FRAME: FrameNodeIndex = FrameNodeIndex(3);
    const BOX_B_FRAME: FrameNodeIndex = FrameNodeIndex(4);

    fn tree_with_viewport_nodes() -> VisualContextTree {
        let mut tree = VisualContextTree::create(transform_data());
        let root_isolation_frame = tree.append_frame(
            FrameData::layer_blending_with(CompositingAndBlendingOperator::Normal),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        tree.root_isolation_frame = Some(root_isolation_frame);
        assert_eq!(root_isolation_frame, ROOT_ISOLATION_FRAME);
        assert_eq!(
            tree.append_spatial(transform(), VISUAL_VIEWPORT_NODE_INDEX),
            VIEWPORT_SCROLL
        );
        tree
    }

    fn tree_with_box_a_followed_by_box_b() -> VisualContextTree {
        let mut tree = tree_with_viewport_nodes();
        assert_eq!(tree.append_spatial(transform(), VIEWPORT_SCROLL), BOX_A_SPATIAL);
        assert_eq!(
            tree.append_frame(clip(10.0), ROOT_ISOLATION_FRAME, BOX_A_SPATIAL),
            BOX_A_CHAIN_FRAME
        );
        assert_eq!(
            tree.append_frame(clip(20.0), BOX_A_CHAIN_FRAME, BOX_A_SPATIAL),
            BOX_A_PATCHED_CHAIN_FRAME
        );
        assert_eq!(
            tree.append_frame(clip(30.0), BOX_A_PATCHED_CHAIN_FRAME, BOX_A_SPATIAL),
            BOX_A_DESCENDANT_FRAME
        );
        assert_eq!(tree.append_spatial(transform(), BOX_A_SPATIAL), BOX_B_SPATIAL);
        assert_eq!(
            tree.append_frame(clip(40.0), BOX_A_DESCENDANT_FRAME, BOX_B_SPATIAL),
            BOX_B_FRAME
        );
        tree
    }

    fn box_a_handles() -> BoxVisualContextNodeHandles {
        BoxVisualContextNodeHandles {
            spatial: vec![BOX_A_SPATIAL],
            chain_frames: vec![BOX_A_CHAIN_FRAME, BOX_A_PATCHED_CHAIN_FRAME],
            descendant_frames: vec![BOX_A_DESCENDANT_FRAME],
        }
    }

    struct ScratchBox {
        spatial_parent: SpatialNodeIndex,
        spatial_count: u32,
        chain_count: u32,
        patched_chain_count: u32,
        descendant_count: u32,
        patched_chain_clip_size: f32,
    }

    impl Default for ScratchBox {
        fn default() -> Self {
            Self {
                spatial_parent: VIEWPORT_SCROLL,
                spatial_count: 1,
                chain_count: 1,
                patched_chain_count: 1,
                descendant_count: 1,
                patched_chain_clip_size: 20.0,
            }
        }
    }

    fn fill_scratch(scratch: &mut BoxNodeScratch<'_>, description: &ScratchBox) -> BoxVisualContextNodeHandles {
        let mut handles = BoxVisualContextNodeHandles::default();
        let mut spatial = description.spatial_parent;
        for _ in 0..description.spatial_count {
            spatial = scratch.append_spatial_node(transform(), spatial);
            handles.spatial.push(spatial);
        }
        let mut frame = ROOT_ISOLATION_FRAME;
        for _ in 0..description.chain_count {
            frame = scratch.append_frame_node(clip(10.0), frame, spatial);
            handles.chain_frames.push(frame);
        }
        for _ in 0..description.patched_chain_count {
            frame = scratch.append_frame_node(clip(description.patched_chain_clip_size), frame, spatial);
            handles.chain_frames.push(frame);
        }
        for _ in 0..description.descendant_count {
            frame = scratch.append_frame_node(clip(30.0), frame, spatial);
            handles.descendant_frames.push(frame);
        }
        handles
    }

    struct Planned {
        placement: BoxNodePlacement,
        spatial: Vec<SpatialNode>,
        frames: Vec<FrameNode>,
        delta: VisualContextTreeDelta,
    }

    fn plan(
        tree: &mut VisualContextTree,
        existing: Option<&BoxVisualContextNodeHandles>,
        description: &ScratchBox,
    ) -> Planned {
        let mut scratch = BoxNodeScratch::new(tree);
        let provisional = fill_scratch(&mut scratch, description);
        let (spatial, frames) = scratch.into_nodes();
        let mut delta = VisualContextTreeDelta::default();
        let placement = plan_box_node_placement(tree, existing, &provisional, &mut delta);
        Planned {
            placement,
            spatial,
            frames,
            delta,
        }
    }

    fn write(
        tree: &mut VisualContextTree,
        existing: Option<&BoxVisualContextNodeHandles>,
        description: &ScratchBox,
    ) -> (BoxNodePlacement, ReconcileOutcome, VisualContextTreeDelta) {
        let planned = plan(tree, existing, description);
        let placement = planned.placement;
        let mut delta = planned.delta;
        let outcome = write_box_nodes(tree, planned.spatial, planned.frames, &placement, existing, &mut delta);
        (placement, outcome, delta)
    }

    #[test]
    fn boxes_without_records_are_numbered_in_write_order_on_a_fresh_tree() {
        let mut tree = tree_with_viewport_nodes();
        let (placement_a, outcome_a, delta_a) = write(&mut tree, None, &ScratchBox::default());
        assert_eq!(placement_a.clone().into_node_handles(), box_a_handles());
        assert!(outcome_a.shape_changed);
        assert!(!delta_a.structural_epoch_changed);
        assert!(delta_a.requires_display_list_recording);

        let description_b = ScratchBox {
            spatial_parent: BOX_A_SPATIAL,
            patched_chain_count: 0,
            descendant_count: 0,
            ..ScratchBox::default()
        };
        let (placement_b, _, _) = write(&mut tree, None, &description_b);
        assert_eq!(placement_b.spatial, vec![BOX_B_SPATIAL]);
        assert_eq!(placement_b.chain_frames, vec![BOX_B_FRAME]);
        assert_eq!(tree.free_slot_count(), 0);
        assert_eq!(tree.quarantined_slot_count(), 0);
    }

    #[test]
    fn a_slot_tombstoned_in_the_same_walk_is_not_handed_to_a_later_box() {
        let mut tree = tree_with_box_a_followed_by_box_b();
        assert!(tree.tombstone_frame_slot(BOX_A_PATCHED_CHAIN_FRAME));
        let description = ScratchBox {
            patched_chain_count: 0,
            descendant_count: 0,
            ..ScratchBox::default()
        };
        let (placement, _, _) = write(&mut tree, None, &description);
        assert_eq!(placement.spatial, vec![SpatialNodeIndex(4)]);
        assert_eq!(placement.chain_frames, vec![FrameNodeIndex(5)]);
        assert_eq!(tree.quarantined_slot_count(), 1);
        assert_eq!(tree.free_slot_count(), 0);
    }

    #[test]
    fn matching_units_keep_their_handles() {
        let mut tree = tree_with_box_a_followed_by_box_b();
        let existing = box_a_handles();
        let planned = plan(&mut tree, Some(&existing), &ScratchBox::default());
        let placement = planned.placement;
        assert_eq!(placement.clone().into_node_handles(), existing);
        assert_eq!(placement.remap_spatial(SpatialNodeIndex(4)), BOX_A_SPATIAL);
        assert_eq!(placement.remap_frame(FrameNodeIndex(5)), BOX_A_CHAIN_FRAME);
        assert_eq!(placement.remap_frame(FrameNodeIndex(6)), BOX_A_PATCHED_CHAIN_FRAME);
        assert_eq!(placement.remap_frame(FrameNodeIndex(7)), BOX_A_DESCENDANT_FRAME);
        assert_eq!(placement.remap_frame(ROOT_ISOLATION_FRAME), ROOT_ISOLATION_FRAME);
        assert_eq!(planned.delta, VisualContextTreeDelta::default());
        assert_eq!(tree.spatial_nodes.len(), 4);
        assert_eq!(tree.frame_nodes.len(), 5);
    }

    #[test]
    fn a_grown_unit_allocates_fresh_slots_at_the_end_when_nothing_is_free() {
        let mut tree = tree_with_box_a_followed_by_box_b();
        let existing = box_a_handles();
        let description = ScratchBox {
            spatial_count: 2,
            chain_count: 2,
            descendant_count: 0,
            ..ScratchBox::default()
        };
        let (placement, outcome, delta) = write(&mut tree, Some(&existing), &description);
        assert_eq!(placement.spatial, vec![BOX_A_SPATIAL, SpatialNodeIndex(4)]);
        assert_eq!(
            placement.chain_frames,
            vec![BOX_A_CHAIN_FRAME, BOX_A_PATCHED_CHAIN_FRAME, FrameNodeIndex(5)]
        );
        assert!(placement.descendant_frames.is_empty());
        assert!(outcome.shape_changed);
        assert_eq!(tree.spatial_nodes[4].parent, BOX_A_SPATIAL);
        assert_eq!(
            tree.frame_nodes[BOX_A_PATCHED_CHAIN_FRAME.0 as usize].parent,
            BOX_A_CHAIN_FRAME
        );
        assert_eq!(tree.frame_nodes[5].parent, BOX_A_PATCHED_CHAIN_FRAME);
        assert_eq!(tree.frame_nodes[5].spatial, SpatialNodeIndex(4));
        assert!(!tree.frame_is_live(BOX_A_DESCENDANT_FRAME));
        assert_eq!(tree.quarantined_slot_count(), 1);
        assert_eq!(tree.free_slot_count(), 0);
        assert!(delta.patched_spatial_node_indices.contains(&4));
        assert!(delta.patched_frame_node_indices.contains(&5));
        assert_eq!(delta.tombstoned_frame_node_indices, vec![BOX_A_DESCENDANT_FRAME.0]);
        assert!(delta.structural_epoch_changed);
        assert!(delta.requires_display_list_recording);
        assert_eq!(tree.live_spatial_node_count, 5);
        assert_eq!(tree.live_frame_node_count, 5);
        tree.debug_assert_slot_accounting();
    }

    #[test]
    fn a_unit_may_reference_a_node_at_a_higher_index() {
        let mut tree = tree_with_box_a_followed_by_box_b();
        let existing = box_a_handles();
        let description = ScratchBox {
            spatial_parent: BOX_B_SPATIAL,
            ..ScratchBox::default()
        };
        let (placement, outcome, delta) = write(&mut tree, Some(&existing), &description);
        assert_eq!(placement.into_node_handles(), existing);
        assert!(outcome.shape_changed);
        assert_eq!(tree.spatial_nodes[BOX_A_SPATIAL.0 as usize].parent, BOX_B_SPATIAL);
        assert_eq!(delta.patched_spatial_node_indices, vec![BOX_A_SPATIAL.0]);
        assert!(delta.tombstoned_spatial_node_indices.is_empty());
        assert!(delta.structural_epoch_changed);
        assert_eq!(tree.spatial_nodes.len(), 4);
    }

    #[test]
    fn writing_in_place_patches_only_the_changed_payloads() {
        let mut tree = tree_with_box_a_followed_by_box_b();
        let existing = box_a_handles();
        let description = ScratchBox {
            patched_chain_clip_size: 25.0,
            ..ScratchBox::default()
        };
        let (_, outcome, delta) = write(&mut tree, Some(&existing), &description);
        assert!(!outcome.shape_changed);
        assert!(outcome.any_payload_changed);
        assert_eq!(delta.patched_spatial_node_indices, Vec::<u32>::new());
        assert_eq!(delta.patched_frame_node_indices, vec![BOX_A_PATCHED_CHAIN_FRAME.0]);
        assert!(delta.tombstoned_frame_node_indices.is_empty());
        assert!(!delta.structural_epoch_changed);
        assert!(!delta.requires_display_list_recording);
        assert_eq!(tree.live_spatial_node_count, 4);
        assert_eq!(tree.live_frame_node_count, 5);
    }

    #[test]
    fn a_shrunken_unit_tombstones_its_surplus_handles_into_quarantine() {
        let mut tree = tree_with_box_a_followed_by_box_b();
        let existing = box_a_handles();
        let description = ScratchBox {
            descendant_count: 0,
            ..ScratchBox::default()
        };
        let (placement, outcome, delta) = write(&mut tree, Some(&existing), &description);
        assert!(placement.descendant_frames.is_empty());
        assert!(outcome.shape_changed);
        assert!(!tree.frame_is_live(BOX_A_DESCENDANT_FRAME));
        assert_eq!(delta.tombstoned_frame_node_indices, vec![BOX_A_DESCENDANT_FRAME.0]);
        assert!(delta.structural_epoch_changed);
        assert!(delta.requires_display_list_recording);
        assert_eq!(tree.quarantined_slot_count(), 1);
        assert_eq!(tree.free_slot_count(), 0);
        assert_eq!(tree.frame_nodes.len(), 5);
        tree.debug_assert_slot_accounting();
    }

    #[test]
    fn a_reused_slot_changes_the_structural_epoch_and_a_fresh_slot_does_not() {
        let mut tree = tree_with_box_a_followed_by_box_b();
        assert!(tree.tombstone_frame_slot(BOX_B_FRAME));
        tree.release_quarantined_slots_after_recording();
        let existing = box_a_handles();
        let description = ScratchBox {
            descendant_count: 2,
            ..ScratchBox::default()
        };
        let (placement, _, delta) = write(&mut tree, Some(&existing), &description);
        assert_eq!(placement.descendant_frames, vec![BOX_A_DESCENDANT_FRAME, BOX_B_FRAME]);
        assert!(delta.structural_epoch_changed);
        assert!(delta.requires_display_list_recording);
        assert_eq!(tree.frame_nodes.len(), 5);

        let mut fresh_tree = tree_with_box_a_followed_by_box_b();
        let (placement, _, delta) = write(&mut fresh_tree, Some(&existing), &description);
        assert_eq!(
            placement.descendant_frames,
            vec![BOX_A_DESCENDANT_FRAME, FrameNodeIndex(5)]
        );
        assert!(!delta.structural_epoch_changed);
        assert!(delta.requires_display_list_recording);
        assert_eq!(fresh_tree.frame_nodes.len(), 6);
    }

    #[test]
    fn provisional_references_between_units_resolve_to_final_handles() {
        let mut tree = tree_with_box_a_followed_by_box_b();
        let existing = box_a_handles();
        let description = ScratchBox {
            spatial_count: 2,
            chain_count: 2,
            ..ScratchBox::default()
        };
        let (placement, _, _) = write(&mut tree, Some(&existing), &description);
        let patched_chain = placement.chain_frames[2];
        assert_eq!(
            tree.frame_nodes[patched_chain.0 as usize].parent,
            placement.chain_frames[1]
        );
        assert_eq!(tree.frame_nodes[patched_chain.0 as usize].spatial, placement.spatial[1]);
        let descendant = placement.descendant_frames[0];
        assert_eq!(tree.frame_nodes[descendant.0 as usize].parent, patched_chain);
        assert_eq!(
            tree.spatial_nodes[placement.spatial[1].0 as usize].parent,
            placement.spatial[0]
        );
    }

    #[test]
    fn a_box_without_a_record_allocates_every_unit() {
        let mut tree = tree_with_box_a_followed_by_box_b();
        let (placement, outcome, mut delta) = write(&mut tree, None, &ScratchBox::default());
        delta.finish();
        assert_eq!(placement.spatial, vec![SpatialNodeIndex(4)]);
        assert_eq!(placement.chain_frames, vec![FrameNodeIndex(5), FrameNodeIndex(6)]);
        assert_eq!(placement.descendant_frames, vec![FrameNodeIndex(7)]);
        assert!(outcome.shape_changed);
        assert_eq!(delta.patched_spatial_node_indices, vec![4]);
        assert_eq!(delta.patched_frame_node_indices, vec![5, 6, 7]);
        assert!(!delta.structural_epoch_changed);
        assert!(delta.requires_display_list_recording);
        assert_eq!(tree.live_spatial_node_count, 5);
        assert_eq!(tree.live_frame_node_count, 8);
        assert!(tree.spatial_is_live(BOX_A_SPATIAL));
        tree.debug_assert_slot_accounting();
    }
}
