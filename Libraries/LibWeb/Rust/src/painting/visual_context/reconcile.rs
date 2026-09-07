/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::delta::VisualContextTreeDelta;
use super::shape::{
    clip_node_shape, clip_payloads_are_equal, effect_node_shape, effect_payloads_are_equal, spatial_node_shape,
    spatial_payloads_are_equal,
};
use super::*;

// Reuse a box's handles in emission order, writing each node before returning its final
// handle. Surplus handles stay live until finish(), then enter the usual quarantine.
pub(crate) struct BoxNodeWriter<'a> {
    tree: &'a mut VisualContextTree,
    existing: &'a BoxVisualContextNodeHandles,
    handles: BoxVisualContextNodeHandles,
    delta: &'a mut VisualContextTreeDelta,
    outcome: ReconcileOutcome,
    building_descendants: bool,
}

impl<'a> BoxNodeWriter<'a> {
    pub(crate) fn new(
        tree: &'a mut VisualContextTree,
        existing: Option<&'a BoxVisualContextNodeHandles>,
        delta: &'a mut VisualContextTreeDelta,
    ) -> Self {
        Self {
            tree,
            existing: existing.unwrap_or(&EMPTY_BOX_VISUAL_CONTEXT_NODE_HANDLES),
            handles: BoxVisualContextNodeHandles::default(),
            delta,
            outcome: ReconcileOutcome::default(),
            building_descendants: false,
        }
    }

    pub(crate) fn begin_descendants(&mut self) {
        debug_assert!(!self.building_descendants);
        self.building_descendants = true;
    }

    pub(crate) fn append_effect_node(
        &mut self,
        data: EffectNodeData,
        parent: EffectNodeIndex,
        spatial: SpatialNodeIndex,
    ) -> EffectNodeIndex {
        debug_assert!(!self.building_descendants, "descendant contexts add no effects");
        debug_assert!(self.tree.spatial_is_live(spatial));
        debug_assert!(self.tree.effect_is_none_or_live(parent));
        write_node(
            self.tree,
            &self.existing.effects,
            &mut self.handles.effects,
            EffectNode::new(data, parent, spatial, None),
            self.delta,
            &mut self.outcome,
        )
    }

    pub(crate) fn finish(mut self) -> (BoxVisualContextNodeHandles, ReconcileOutcome) {
        retire_surplus::<SpatialNode>(
            self.tree,
            &self.existing.spatial,
            self.handles.spatial.len(),
            self.delta,
            &mut self.outcome,
        );
        retire_surplus::<ClipNode>(
            self.tree,
            &self.existing.chain_clips,
            self.handles.chain_clips.len(),
            self.delta,
            &mut self.outcome,
        );
        retire_surplus::<ClipNode>(
            self.tree,
            &self.existing.descendant_clips,
            self.handles.descendant_clips.len(),
            self.delta,
            &mut self.outcome,
        );
        retire_surplus::<EffectNode>(
            self.tree,
            &self.existing.effects,
            self.handles.effects.len(),
            self.delta,
            &mut self.outcome,
        );
        (self.handles, self.outcome)
    }
}

impl VisualContextNodeSink for BoxNodeWriter<'_> {
    fn append_spatial_node(&mut self, data: SpatialData, parent: SpatialNodeIndex) -> SpatialNodeIndex {
        debug_assert!(self.tree.spatial_is_live(parent));
        write_node(
            self.tree,
            &self.existing.spatial,
            &mut self.handles.spatial,
            SpatialNode { data, parent },
            self.delta,
            &mut self.outcome,
        )
    }

    fn append_clip_node(
        &mut self,
        data: ClipNodeData,
        parent: ClipNodeIndex,
        spatial: SpatialNodeIndex,
    ) -> ClipNodeIndex {
        debug_assert!(self.tree.spatial_is_live(spatial));
        debug_assert!(self.tree.clip_is_none_or_live(parent));
        let (existing, handles) = if self.building_descendants {
            (&self.existing.descendant_clips, &mut self.handles.descendant_clips)
        } else {
            (&self.existing.chain_clips, &mut self.handles.chain_clips)
        };
        write_node(
            self.tree,
            existing,
            handles,
            ClipNode::new(data, parent, spatial),
            self.delta,
            &mut self.outcome,
        )
    }

    fn spatial_node_at(&self, index: SpatialNodeIndex) -> &SpatialNode {
        &self.tree.spatial_nodes[index.0 as usize]
    }

    fn clip_node_at(&self, index: ClipNodeIndex) -> &ClipNode {
        &self.tree.clip_nodes[index.0 as usize]
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct ReconcileOutcome {
    pub shape_changed: bool,
}

// A slot whose shape differs is repurposed, one whose payload differs is patched, and
// surplus slots the box no longer needs are tombstoned.
trait ReconciledNode: SlotNode {
    fn slots(tree: &VisualContextTree) -> &[Self];
    fn allocate(tree: &mut VisualContextTree) -> (Self::Index, bool);
    fn same_shape(&self, other: &Self) -> bool;
    fn same_payload(&self, other: &Self) -> bool;
    fn replace(tree: &mut VisualContextTree, index: Self::Index, node: Self) -> bool;
    fn tombstone_slot(tree: &mut VisualContextTree, index: Self::Index) -> bool;
    fn patch_requires_recording(&self) -> bool {
        false
    }
}

impl ReconciledNode for SpatialNode {
    fn allocate(tree: &mut VisualContextTree) -> (SpatialNodeIndex, bool) {
        tree.allocate_spatial_slot()
    }
    fn slots(tree: &VisualContextTree) -> &[Self] {
        &tree.spatial_nodes
    }
    fn same_shape(&self, other: &Self) -> bool {
        spatial_node_shape(self) == spatial_node_shape(other)
    }
    fn same_payload(&self, other: &Self) -> bool {
        spatial_payloads_are_equal(&self.data, &other.data)
    }
    fn replace(tree: &mut VisualContextTree, index: SpatialNodeIndex, node: Self) -> bool {
        tree.replace_spatial_node(index, node)
    }
    fn tombstone_slot(tree: &mut VisualContextTree, index: SpatialNodeIndex) -> bool {
        tree.tombstone_spatial_slot(index)
    }
}

impl ReconciledNode for ClipNode {
    fn allocate(tree: &mut VisualContextTree) -> (ClipNodeIndex, bool) {
        tree.allocate_clip_slot()
    }
    fn slots(tree: &VisualContextTree) -> &[Self] {
        &tree.clip_nodes
    }
    fn same_shape(&self, other: &Self) -> bool {
        clip_node_shape(self) == clip_node_shape(other)
    }
    fn same_payload(&self, other: &Self) -> bool {
        clip_payloads_are_equal(&self.data, &other.data)
    }
    fn replace(tree: &mut VisualContextTree, index: ClipNodeIndex, node: Self) -> bool {
        tree.replace_clip_node(index, node)
    }
    fn tombstone_slot(tree: &mut VisualContextTree, index: ClipNodeIndex) -> bool {
        tree.tombstone_clip_slot(index)
    }
}

impl ReconciledNode for EffectNode {
    fn allocate(tree: &mut VisualContextTree) -> (EffectNodeIndex, bool) {
        tree.allocate_effect_slot()
    }
    fn slots(tree: &VisualContextTree) -> &[Self] {
        &tree.effect_nodes
    }
    fn same_shape(&self, other: &Self) -> bool {
        effect_node_shape(self) == effect_node_shape(other)
    }
    fn same_payload(&self, other: &Self) -> bool {
        effect_payloads_are_equal(&self.data, &other.data)
    }
    fn replace(tree: &mut VisualContextTree, index: EffectNodeIndex, mut node: Self) -> bool {
        let previous = &tree.effect_nodes[index.0 as usize];
        if previous.data.is_live() {
            node.resolved_output_clip = previous.resolved_output_clip;
        }
        tree.replace_effect_node(index, node)
    }
    fn tombstone_slot(tree: &mut VisualContextTree, index: EffectNodeIndex) -> bool {
        tree.tombstone_effect_slot(index)
    }
    // A mask's content is recorded per display list, so a changed mask needs a recording.
    fn patch_requires_recording(&self) -> bool {
        matches!(self.data, EffectNodeData::Mask(_))
    }
}

fn write_node<N: ReconciledNode>(
    tree: &mut VisualContextTree,
    existing: &[N::Index],
    handles: &mut Vec<N::Index>,
    node: N,
    delta: &mut VisualContextTreeDelta,
    outcome: &mut ReconcileOutcome,
) -> N::Index {
    let handle = match existing.get(handles.len()) {
        Some(&handle) => handle,
        None => {
            let (handle, reused) = N::allocate(tree);
            delta.note_allocated(reused);
            handle
        }
    };
    let current = &N::slots(tree)[N::raw(handle) as usize];
    let was_live = current.is_live();
    let shape_matches = was_live && current.same_shape(&node);
    let payload_matches = shape_matches && current.same_payload(&node);
    if !shape_matches {
        outcome.shape_changed = true;
        if was_live {
            delta.note_repurposed_in_place();
        }
    }
    if !payload_matches {
        if node.patch_requires_recording() {
            delta.requires_display_list_recording = true;
        }
        N::replace(tree, handle, node);
    }
    handles.push(handle);
    handle
}

fn retire_surplus<N: ReconciledNode>(
    tree: &mut VisualContextTree,
    existing: &[N::Index],
    used: usize,
    delta: &mut VisualContextTreeDelta,
    outcome: &mut ReconcileOutcome,
) {
    if used < existing.len() {
        outcome.shape_changed = true;
    }
    for &handle in existing.get(used..).unwrap_or(&[]) {
        if N::tombstone_slot(tree, handle) {
            delta.note_tombstoned();
        }
    }
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

    fn clip(size: f32) -> ClipNodeData {
        ClipNodeData::rect_clip(FloatRect::new(0.0, 0.0, size, size))
    }

    fn effects() -> EffectNodeData {
        EffectNodeData::layer_blending_with(CompositingAndBlendingOperator::Normal)
    }

    const VIEWPORT_SCROLL: SpatialNodeIndex = SpatialNodeIndex(1);
    const ROOT_ISOLATION_EFFECT: EffectNodeIndex = EffectNodeIndex(0);
    const BOX_A_SPATIAL: SpatialNodeIndex = SpatialNodeIndex(2);
    const BOX_B_SPATIAL: SpatialNodeIndex = SpatialNodeIndex(3);
    const BOX_A_CHAIN_CLIP: ClipNodeIndex = ClipNodeIndex(0);
    const BOX_A_PATCHED_CHAIN_CLIP: ClipNodeIndex = ClipNodeIndex(1);
    const BOX_A_DESCENDANT_CLIP: ClipNodeIndex = ClipNodeIndex(2);
    const BOX_B_CLIP: ClipNodeIndex = ClipNodeIndex(3);
    const BOX_A_EFFECT: EffectNodeIndex = EffectNodeIndex(1);

    fn tree_with_viewport_nodes() -> VisualContextTree {
        let mut tree = VisualContextTree::create(transform_data());
        let root_isolation_effect = tree.append_effect(
            effects(),
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );
        assert_eq!(root_isolation_effect, ROOT_ISOLATION_EFFECT);
        tree.root_isolation_effect = Some(root_isolation_effect);

        assert_eq!(
            tree.append_spatial(transform(), VISUAL_VIEWPORT_NODE_INDEX),
            VIEWPORT_SCROLL
        );
        tree
    }

    // Box A: two chain clips, an effect under the second, an overflow clip for descendants.
    // Box B: one clip under A's overflow clip.
    fn tree_with_box_a_followed_by_box_b() -> VisualContextTree {
        let mut tree = tree_with_viewport_nodes();
        assert_eq!(tree.append_spatial(transform(), VIEWPORT_SCROLL), BOX_A_SPATIAL);
        assert_eq!(
            tree.append_clip(clip(10.0), ClipNodeIndex::NONE, BOX_A_SPATIAL),
            BOX_A_CHAIN_CLIP
        );
        assert_eq!(
            tree.append_clip(clip(20.0), BOX_A_CHAIN_CLIP, BOX_A_SPATIAL),
            BOX_A_PATCHED_CHAIN_CLIP
        );
        assert_eq!(
            tree.append_effect(
                effects(),
                ROOT_ISOLATION_EFFECT,
                BOX_A_SPATIAL,
                BOX_A_PATCHED_CHAIN_CLIP
            ),
            BOX_A_EFFECT
        );
        assert_eq!(
            tree.append_clip(clip(30.0), BOX_A_PATCHED_CHAIN_CLIP, BOX_A_SPATIAL),
            BOX_A_DESCENDANT_CLIP
        );
        assert_eq!(tree.append_spatial(transform(), BOX_A_SPATIAL), BOX_B_SPATIAL);
        assert_eq!(
            tree.append_clip(clip(40.0), BOX_A_DESCENDANT_CLIP, BOX_B_SPATIAL),
            BOX_B_CLIP
        );
        tree
    }

    fn box_a_handles() -> BoxVisualContextNodeHandles {
        BoxVisualContextNodeHandles {
            spatial: vec![BOX_A_SPATIAL],
            chain_clips: vec![BOX_A_CHAIN_CLIP, BOX_A_PATCHED_CHAIN_CLIP],
            descendant_clips: vec![BOX_A_DESCENDANT_CLIP],
            effects: vec![BOX_A_EFFECT],
        }
    }

    struct BoxDescription {
        spatial_parent: SpatialNodeIndex,
        inherited_context: ContextRef,
        spatial_count: u32,
        chain_count: u32,
        patched_chain_count: u32,
        effect_count: u32,
        descendant_count: u32,
        patched_chain_clip_size: f32,
    }

    impl Default for BoxDescription {
        fn default() -> Self {
            Self {
                spatial_parent: VIEWPORT_SCROLL,
                inherited_context: ContextRef {
                    effect: ROOT_ISOLATION_EFFECT,
                    ..ContextRef::default()
                },
                spatial_count: 1,
                chain_count: 1,
                patched_chain_count: 1,
                effect_count: 1,
                descendant_count: 1,
                patched_chain_clip_size: 20.0,
            }
        }
    }

    fn write(
        tree: &mut VisualContextTree,
        existing: Option<&BoxVisualContextNodeHandles>,
        description: &BoxDescription,
    ) -> (BoxVisualContextNodeHandles, ReconcileOutcome, VisualContextTreeDelta) {
        let mut delta = VisualContextTreeDelta::default();
        let mut writer = BoxNodeWriter::new(tree, existing, &mut delta);
        let mut context = ContextRef {
            spatial: description.spatial_parent,
            ..description.inherited_context
        };
        for _ in 0..description.spatial_count {
            context = writer.append_spatial_node_under(context, transform());
        }
        for _ in 0..description.chain_count {
            context = writer.append_clip_node_under(context, clip(10.0));
        }
        for _ in 0..description.patched_chain_count {
            context = writer.append_clip_node_under(context, clip(description.patched_chain_clip_size));
        }
        for _ in 0..description.effect_count {
            context.effect = writer.append_effect_node(effects(), context.effect, context.spatial);
        }
        writer.begin_descendants();
        for _ in 0..description.descendant_count {
            context = writer.append_clip_node_under(context, clip(30.0));
        }
        let (handles, outcome) = writer.finish();
        (handles, outcome, delta)
    }

    fn box_b_description() -> BoxDescription {
        BoxDescription {
            spatial_parent: BOX_A_SPATIAL,
            inherited_context: ContextRef {
                clip: BOX_A_DESCENDANT_CLIP,
                effect: BOX_A_EFFECT,
                ..ContextRef::default()
            },
            patched_chain_count: 0,
            effect_count: 0,
            descendant_count: 0,
            ..BoxDescription::default()
        }
    }

    #[test]
    fn boxes_without_records_are_numbered_in_write_order_on_a_fresh_tree() {
        let mut tree = tree_with_viewport_nodes();
        let (handles_a, outcome_a, delta_a) = write(&mut tree, None, &BoxDescription::default());
        assert_eq!(handles_a, box_a_handles());
        assert!(outcome_a.shape_changed);
        assert!(!delta_a.structural_epoch_changed);
        assert!(delta_a.requires_display_list_recording);

        let (handles_b, _, _) = write(&mut tree, None, &box_b_description());
        assert_eq!(handles_b.spatial, vec![BOX_B_SPATIAL]);
        assert_eq!(handles_b.chain_clips, vec![BOX_B_CLIP]);
        assert!(handles_b.effects.is_empty());
        assert_eq!(tree.free_slot_count(), 0);
        assert_eq!(tree.quarantined_slot_count(), 0);
    }

    #[test]
    fn a_slot_tombstoned_in_the_same_walk_is_not_handed_to_a_later_box() {
        let mut tree = tree_with_box_a_followed_by_box_b();

        assert!(tree.tombstone_clip_slot(BOX_A_PATCHED_CHAIN_CLIP));
        let (handles, _, _) = write(&mut tree, None, &box_b_description());
        assert_eq!(handles.spatial, vec![SpatialNodeIndex(4)]);
        assert_eq!(handles.chain_clips, vec![ClipNodeIndex(4)]);
        assert_eq!(tree.quarantined_slot_count(), 1);
        assert_eq!(tree.free_slot_count(), 0);
    }

    #[test]
    fn matching_units_keep_their_handles() {
        let mut tree = tree_with_box_a_followed_by_box_b();
        let existing = box_a_handles();
        let (handles, outcome, delta) = write(&mut tree, Some(&existing), &BoxDescription::default());
        assert_eq!(handles, existing);
        assert!(!outcome.shape_changed);
        assert_eq!(delta, VisualContextTreeDelta::default());
        assert_eq!(tree.spatial_nodes.len(), 4);
        assert_eq!(tree.clip_nodes.len(), 4);
        assert_eq!(tree.effect_nodes.len(), 2);
    }

    #[test]
    fn a_grown_unit_allocates_fresh_slots_at_the_end_when_nothing_is_free() {
        let mut tree = tree_with_box_a_followed_by_box_b();
        let existing = box_a_handles();
        let description = BoxDescription {
            spatial_count: 2,
            chain_count: 2,
            descendant_count: 0,
            ..BoxDescription::default()
        };
        let (handles, outcome, delta) = write(&mut tree, Some(&existing), &description);
        assert_eq!(handles.spatial, vec![BOX_A_SPATIAL, SpatialNodeIndex(4)]);
        assert_eq!(
            handles.chain_clips,
            vec![BOX_A_CHAIN_CLIP, BOX_A_PATCHED_CHAIN_CLIP, ClipNodeIndex(4)]
        );
        assert!(handles.descendant_clips.is_empty());
        assert_eq!(handles.effects, vec![BOX_A_EFFECT]);
        assert!(outcome.shape_changed);
        assert_eq!(tree.spatial_nodes[4].parent, BOX_A_SPATIAL);
        assert_eq!(
            tree.clip_nodes[BOX_A_PATCHED_CHAIN_CLIP.0 as usize].parent,
            BOX_A_CHAIN_CLIP
        );
        assert_eq!(tree.clip_nodes[4].parent, BOX_A_PATCHED_CHAIN_CLIP);
        assert_eq!(tree.clip_nodes[4].spatial, SpatialNodeIndex(4));
        assert_eq!(
            tree.effect_nodes[BOX_A_EFFECT.0 as usize].output_clip(),
            BOX_A_PATCHED_CHAIN_CLIP
        );
        assert_eq!(tree.effect_nodes[BOX_A_EFFECT.0 as usize].spatial, SpatialNodeIndex(4));
        assert!(!tree.clip_is_live(BOX_A_DESCENDANT_CLIP));
        assert_eq!(tree.quarantined_slot_count(), 1);
        assert_eq!(tree.free_slot_count(), 0);
        assert!(delta.tombstoned_any_node);
        assert!(delta.structural_epoch_changed);
        assert!(delta.requires_display_list_recording);
        assert_eq!(tree.live_spatial_node_count(), 5);
        assert_eq!(tree.live_clip_node_count(), 4);
        assert_eq!(tree.live_effect_node_count(), 2);
        tree.debug_assert_slot_accounting();
    }

    #[test]
    fn a_unit_may_reference_a_node_at_a_higher_index() {
        let mut tree = tree_with_box_a_followed_by_box_b();
        let existing = box_a_handles();
        let description = BoxDescription {
            spatial_parent: BOX_B_SPATIAL,
            ..BoxDescription::default()
        };
        let (handles, outcome, delta) = write(&mut tree, Some(&existing), &description);
        assert_eq!(handles, existing);
        assert!(outcome.shape_changed);
        assert_eq!(tree.spatial_nodes[BOX_A_SPATIAL.0 as usize].parent, BOX_B_SPATIAL);
        assert!(!delta.tombstoned_any_node);
        assert!(delta.structural_epoch_changed);
        assert!(delta.requires_display_list_recording);
        assert_eq!(tree.spatial_nodes.len(), 4);
    }

    #[test]
    fn writing_a_clip_value_in_place_needs_no_recording_or_epoch_change() {
        let mut tree = tree_with_box_a_followed_by_box_b();
        let existing = box_a_handles();
        let description = BoxDescription {
            patched_chain_clip_size: 25.0,
            ..BoxDescription::default()
        };
        let (_, outcome, delta) = write(&mut tree, Some(&existing), &description);
        assert!(!outcome.shape_changed);
        let ClipNodeData::Rect(updated_clip) = &tree.clip_nodes[BOX_A_PATCHED_CHAIN_CLIP.0 as usize].data else {
            panic!("the updated clip remains a rectangle");
        };
        assert_eq!(updated_clip.rect, FloatRect::new(0.0, 0.0, 25.0, 25.0));
        assert!(!delta.tombstoned_any_node);
        assert!(!delta.structural_epoch_changed);
        assert!(!delta.requires_display_list_recording);
        assert_eq!(tree.live_spatial_node_count(), 4);
        assert_eq!(tree.live_clip_node_count(), 4);
        assert_eq!(tree.live_effect_node_count(), 2);
    }

    #[test]
    fn rebuilding_an_effect_preserves_its_resolved_clip_until_finalization() {
        let mut tree = tree_with_box_a_followed_by_box_b();
        let constraints = [
            EffectClipConstraint {
                effect: BOX_A_EFFECT,
                clip: BOX_A_PATCHED_CHAIN_CLIP,
            },
            EffectClipConstraint {
                effect: BOX_A_EFFECT,
                clip: ClipNodeIndex::NONE,
            },
        ];
        assert!(tree.resolve_effect_output_clips(&constraints));
        let existing = BoxVisualContextNodeHandles {
            effects: vec![BOX_A_EFFECT],
            ..BoxVisualContextNodeHandles::default()
        };
        let mut delta = VisualContextTreeDelta::default();
        let mut writer = BoxNodeWriter::new(&mut tree, Some(&existing), &mut delta);
        let effect = writer.append_effect_node(
            EffectNodeData::Effects(EffectsData {
                opacity: 0.7,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
            }),
            ROOT_ISOLATION_EFFECT,
            BOX_A_SPATIAL,
        );
        let (_, outcome) = writer.finish();

        assert_eq!(effect, BOX_A_EFFECT);
        assert_eq!(tree.effects_opacity(effect), Some(0.7));
        assert_eq!(tree.effect_nodes[effect.0 as usize].output_clip(), ClipNodeIndex::NONE);
        assert!(!outcome.shape_changed);
        assert_eq!(delta, VisualContextTreeDelta::default());
        assert!(!tree.resolve_effect_output_clips(&constraints));
        // Removing the escape changes only the derived result, without rebuilding the effect.
        assert!(tree.resolve_effect_output_clips(&constraints[..1]));
        assert_eq!(
            tree.effect_nodes[effect.0 as usize].output_clip(),
            BOX_A_PATCHED_CHAIN_CLIP
        );
    }

    #[test]
    fn new_and_recycled_effects_have_no_previous_output_clip() {
        let mut tree = tree_with_box_a_followed_by_box_b();
        for recycled in [false, true] {
            let mut delta = VisualContextTreeDelta::default();
            let mut writer = BoxNodeWriter::new(&mut tree, None, &mut delta);
            let effect = writer.append_effect_node(effects(), ROOT_ISOLATION_EFFECT, BOX_A_SPATIAL);
            writer.finish();
            let context = ContextRef {
                spatial: BOX_A_SPATIAL,
                clip: BOX_A_CHAIN_CLIP,
                effect,
            };
            assert!(!tree.context_is_valid(context));
            assert!(!tree.node_references_are_consistent());
            assert!(!tree.resolve_effect_output_clips(&[EffectClipConstraint {
                effect,
                clip: context.clip,
            }]));
            assert!(tree.context_is_valid(context));
            assert!(tree.node_references_are_consistent());
            assert_eq!(delta.structural_epoch_changed, recycled);
            assert!(delta.requires_display_list_recording);
            assert_eq!(effect, EffectNodeIndex(2));
            assert!(tree.tombstone_effect_slot(effect));
            tree.release_quarantined_slots_after_recording();
        }
    }

    #[test]
    fn a_shrunken_unit_tombstones_its_surplus_handles_into_quarantine() {
        let mut tree = tree_with_box_a_followed_by_box_b();
        let existing = box_a_handles();
        let description = BoxDescription {
            descendant_count: 0,
            ..BoxDescription::default()
        };
        let (handles, outcome, delta) = write(&mut tree, Some(&existing), &description);
        assert!(handles.descendant_clips.is_empty());
        assert!(outcome.shape_changed);
        assert!(!tree.clip_is_live(BOX_A_DESCENDANT_CLIP));
        assert!(delta.tombstoned_any_node);
        assert!(delta.structural_epoch_changed);
        assert!(delta.requires_display_list_recording);
        assert_eq!(tree.quarantined_slot_count(), 1);
        assert_eq!(tree.free_slot_count(), 0);
        assert_eq!(tree.clip_nodes.len(), 4);
        tree.debug_assert_slot_accounting();
    }

    #[test]
    fn a_reused_slot_changes_the_structural_epoch_and_a_fresh_slot_does_not() {
        let mut tree = tree_with_box_a_followed_by_box_b();

        assert!(tree.tombstone_clip_slot(BOX_B_CLIP));
        tree.release_quarantined_slots_after_recording();
        let existing = box_a_handles();
        let description = BoxDescription {
            descendant_count: 2,
            ..BoxDescription::default()
        };
        let (handles, _, delta) = write(&mut tree, Some(&existing), &description);
        assert_eq!(handles.descendant_clips, vec![BOX_A_DESCENDANT_CLIP, BOX_B_CLIP]);
        assert!(delta.structural_epoch_changed);
        assert!(delta.requires_display_list_recording);
        assert_eq!(tree.clip_nodes.len(), 4);

        let mut fresh_tree = tree_with_box_a_followed_by_box_b();
        let (handles, _, delta) = write(&mut fresh_tree, Some(&existing), &description);
        assert_eq!(handles.descendant_clips, vec![BOX_A_DESCENDANT_CLIP, ClipNodeIndex(4)]);
        assert!(!delta.structural_epoch_changed);
        assert!(delta.requires_display_list_recording);
        assert_eq!(fresh_tree.clip_nodes.len(), 5);
    }

    #[test]
    fn emitted_nodes_are_readable_by_their_final_handles() {
        let mut tree = tree_with_box_a_followed_by_box_b();
        let existing = box_a_handles();
        let mut delta = VisualContextTreeDelta::default();
        let mut writer = BoxNodeWriter::new(&mut tree, Some(&existing), &mut delta);
        let context = ContextRef {
            spatial: VIEWPORT_SCROLL,
            effect: ROOT_ISOLATION_EFFECT,
            ..ContextRef::default()
        };
        let context = writer.append_spatial_node_under(context, transform());
        assert_eq!(context.spatial, BOX_A_SPATIAL);
        let transformed = writer.append_spatial_node_under(
            context,
            SpatialData::Transform(TransformData {
                sorting_context_root_index: Some(context.spatial),
                ..transform_data()
            }),
        );
        assert_eq!(transformed.spatial, SpatialNodeIndex(4));
        let spatial_node = writer.spatial_node_at(transformed.spatial);
        assert_eq!(spatial_node.parent, BOX_A_SPATIAL);
        let SpatialData::Transform(data) = &spatial_node.data else {
            panic!("the appended node is a transform");
        };
        assert_eq!(data.sorting_context_root_index, Some(BOX_A_SPATIAL));

        let first_clip = writer.append_clip_node_under(transformed, clip(15.0));
        assert_eq!(first_clip.clip, BOX_A_CHAIN_CLIP);
        let ClipNodeData::Rect(data) = &writer.clip_node_at(first_clip.clip).data else {
            panic!("the updated clip remains a rectangle");
        };
        assert_eq!(data.rect, FloatRect::new(0.0, 0.0, 15.0, 15.0));
        let second_clip = writer.append_clip_node_under(first_clip, clip(20.0));
        assert_eq!(second_clip.clip, BOX_A_PATCHED_CHAIN_CLIP);
        assert_eq!(writer.clip_node_at(second_clip.clip).spatial, transformed.spatial);
        assert!(writer.clip_is_ancestor_or_self(first_clip.clip, second_clip.clip));
        let third_clip = writer.append_clip_node_under(second_clip, clip(25.0));
        assert_eq!(third_clip.clip, ClipNodeIndex(4));
        assert_eq!(writer.clip_node_at(third_clip.clip).parent, second_clip.clip);
        let own_context = ContextRef {
            effect: writer.append_effect_node(effects(), third_clip.effect, third_clip.spatial),
            ..third_clip
        };
        assert_eq!(own_context.effect, BOX_A_EFFECT);
        assert!(writer.tree.context_is_valid(own_context));
        writer.begin_descendants();
        let descendant_context = writer.append_clip_node_under(own_context, clip(30.0));
        assert_eq!(descendant_context.clip, BOX_A_DESCENDANT_CLIP);
        assert_eq!(writer.clip_node_at(descendant_context.clip).parent, own_context.clip);
        assert!(writer.tree.context_is_valid(descendant_context));

        let (handles, _) = writer.finish();
        assert!(tree.resolve_effect_output_clips(&[EffectClipConstraint {
            effect: own_context.effect,
            clip: own_context.clip,
        }]));
        assert_eq!(handles.spatial, vec![BOX_A_SPATIAL, transformed.spatial]);
        assert_eq!(
            handles.chain_clips,
            vec![BOX_A_CHAIN_CLIP, BOX_A_PATCHED_CHAIN_CLIP, third_clip.clip]
        );
        assert_eq!(handles.descendant_clips, existing.descendant_clips);
        assert_eq!(handles.effects, existing.effects);
        assert_eq!(tree.effect_nodes[BOX_A_EFFECT.0 as usize].parent, ROOT_ISOLATION_EFFECT);
        assert_eq!(
            tree.effect_nodes[BOX_A_EFFECT.0 as usize].output_clip(),
            own_context.clip
        );
        assert_eq!(tree.effect_nodes[BOX_A_EFFECT.0 as usize].spatial, transformed.spatial);
        tree.debug_assert_slot_accounting();
    }

    #[test]
    fn a_box_without_a_record_allocates_every_unit() {
        let mut tree = tree_with_box_a_followed_by_box_b();
        let (handles, outcome, delta) = write(&mut tree, None, &BoxDescription::default());
        assert_eq!(handles.spatial, vec![SpatialNodeIndex(4)]);
        assert_eq!(handles.chain_clips, vec![ClipNodeIndex(4), ClipNodeIndex(5)]);
        assert_eq!(handles.descendant_clips, vec![ClipNodeIndex(6)]);
        assert_eq!(handles.effects, vec![EffectNodeIndex(2)]);
        assert!(outcome.shape_changed);
        assert!(!delta.tombstoned_any_node);
        assert!(!delta.structural_epoch_changed);
        assert!(delta.requires_display_list_recording);
        assert_eq!(tree.live_spatial_node_count(), 5);
        assert_eq!(tree.live_clip_node_count(), 7);
        assert_eq!(tree.live_effect_node_count(), 3);
        assert!(tree.spatial_is_live(BOX_A_SPATIAL));
        tree.debug_assert_slot_accounting();
    }
}
