/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::box_build::{
    AnchorScrollShiftResolver, BoxBuildEnvironment, PaintableVisualContextAssignment, build_box_visual_context_nodes,
};
use super::delta::VisualContextTreeDelta;
use super::dirty::{
    BoxDirtyBits, VisualContextBoxDirtyKind, VisualContextDirtySet, VisualContextGlobalRebuildReason,
    VisualContextUpdateScope,
};
use super::reconcile::{BoxNodeScratch, plan_box_node_placement, write_box_nodes};
use super::refresh::compute_sticky_data;
use super::scroll_state::ScrollState;
use super::*;
use crate::layout::node_data::{NodeFlag, NodeKind, NodeSlotId};
use crate::painting::host::{FfiRootBackgroundSource, FfiVisualContextHostCallbacks, FfiVisualContextTreeInputs};
use crate::painting::paint_order;
use crate::painting::paintable_rows::PaintableRowsRead;
use std::collections::{HashMap, HashSet};
use std::rc::Rc;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct ChildCascade {
    input_changed: bool,
    geometry_walk: bool,
}

impl ChildCascade {
    fn visits_every_child(self) -> bool {
        self.input_changed || self.geometry_walk
    }
}

pub(crate) struct IncrementalUpdateOutcome {
    pub delta: VisualContextTreeDelta,
    pub assignments: Vec<PaintableVisualContextAssignment>,
    pub mask_node_owners_changed: bool,
}

pub(crate) enum IncrementalUpdateResult {
    Applied(IncrementalUpdateOutcome),
    NeedsFullBuild(VisualContextGlobalRebuildReason),
}

fn box_is_inside_svg_resource_subtree(layout_arena: &impl PaintableRowsRead, slot: NodeSlotId) -> bool {
    let mut node = Some(slot);
    while let Some(current) = node {
        if matches!(
            layout_arena.node_kind_if_live(current),
            Some(NodeKind::SVGMaskBox | NodeKind::SVGClipBox | NodeKind::SVGPatternBox)
        ) {
            return true;
        }
        node = layout_arena.node_parent_if_live(current);
    }
    false
}

#[derive(Default)]
struct WorkPlan {
    work: HashMap<NodeSlotId, BoxDirtyBits>,
    revalidate_children_of: HashSet<NodeSlotId>,
    ancestors_of_work: HashSet<NodeSlotId>,
}

fn expand_dirty_entries(
    layout_arena: &impl PaintableRowsRead,
    dirty: &VisualContextDirtySet,
    root_background_source: FfiRootBackgroundSource,
) -> Result<WorkPlan, VisualContextGlobalRebuildReason> {
    let mut work: HashMap<NodeSlotId, BoxDirtyBits> = HashMap::new();
    for (slot, bits) in &dirty.boxes {
        if !layout_arena.paintable_row_is_populated(*slot) {
            continue;
        }
        if box_is_inside_svg_resource_subtree(layout_arena, *slot) {
            return Err(VisualContextGlobalRebuildReason::SvgResourceSubtreeChanged);
        }
        let mut bits = *bits;
        if layout_arena.node_kind_if_live(*slot) == Some(NodeKind::SVGSVGBox) && !bits.is_value_only() {
            bits.insert(VisualContextBoxDirtyKind::MovedWithDescendants);
        }
        work.entry(*slot).or_default().merge(bits);
        let is_body = layout_arena.node_flags_if_live(*slot) & NodeFlag::IsBody as u32 != 0;
        if is_body
            && root_background_source.use_body_background_properties
            && let Some(html) = paint_order::paint_parent(layout_arena, *slot)
        {
            work.entry(html)
                .or_default()
                .insert(VisualContextBoxDirtyKind::StyleStructuralChange);
        }
        if layout_arena.node_flags_if_live(*slot) & NodeFlag::IsHtmlHtmlElement as u32 != 0
            && root_background_source.use_body_background_properties
            && layout_arena.paintable_row_is_populated(root_background_source.body_layout_node)
        {
            work.entry(root_background_source.body_layout_node)
                .or_default()
                .insert(VisualContextBoxDirtyKind::StyleStructuralChange);
        }
    }
    let mut revalidate_children_of = HashSet::new();
    for removed in &dirty.removed {
        if layout_arena.paintable_row_is_populated(removed.former_paint_parent) {
            revalidate_children_of.insert(removed.former_paint_parent);
        }
    }
    let mut ancestors_of_work = HashSet::new();
    for slot in work.keys().copied().chain(revalidate_children_of.iter().copied()) {
        let mut ancestor = paint_order::paint_parent(layout_arena, slot);
        while let Some(current) = ancestor {
            if !ancestors_of_work.insert(current) {
                break;
            }
            ancestor = paint_order::paint_parent(layout_arena, current);
        }
    }
    Ok(WorkPlan {
        work,
        revalidate_children_of,
        ancestors_of_work,
    })
}

fn tombstone_removed_blocks(
    tree: &mut VisualContextTree,
    dirty: &VisualContextDirtySet,
    delta: &mut VisualContextTreeDelta,
) {
    for removed in &dirty.removed {
        for index in &removed.node_handles.spatial {
            if tree.tombstone_spatial_slot(*index) {
                delta.note_tombstoned_spatial(index.0);
            }
        }
        for index in removed.node_handles.frame_handles() {
            if tree.tombstone_frame_slot(index) {
                delta.note_tombstoned_frame(index.0);
            }
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum RegisteredScrollLikeNode {
    Scroll,
    Sticky,
}

pub(crate) fn register_scroll_like_node(
    layout_arena: &impl PaintableRowsRead,
    tree: &mut VisualContextTree,
    scroll_state: &mut ScrollState,
    node_index: SpatialNodeIndex,
) -> Option<RegisteredScrollLikeNode> {
    let index = node_index.0 as usize;
    let (is_sticky, owner_paintable, registry_parent_node) = match &tree.spatial_nodes[index].data {
        SpatialData::Scroll(scroll) => (false, scroll.owner_paintable, scroll.registry_parent_node),
        SpatialData::Sticky(sticky) => (true, sticky.owner_paintable, sticky.registry_parent_node),
        _ => return None,
    };
    let parent_slot = tree.scroll_state_slot_for_node(registry_parent_node);
    if is_sticky {
        let slot = scroll_state.register_sticky_node(node_index, owner_paintable, parent_slot);
        if let SpatialData::Sticky(sticky) = &mut tree.spatial_nodes[index].data {
            sticky.state_slot = slot;
        }
        Some(RegisteredScrollLikeNode::Sticky)
    } else {
        let slot = scroll_state.register_scroll_node(node_index, owner_paintable, parent_slot);
        if let SpatialData::Scroll(scroll) = &mut tree.spatial_nodes[index].data {
            scroll.state_slot = slot;
        }
        if layout_arena.node_kind_if_live(owner_paintable) != Some(NodeKind::Viewport)
            && let Some(style) = layout_arena.node_style_if_live(owner_paintable)
        {
            use crate::css::css_enums::overflow;
            let box_values = style.box_values();
            if matches!(box_values.overflow_x, overflow::AUTO | overflow::SCROLL)
                || matches!(box_values.overflow_y, overflow::AUTO | overflow::SCROLL)
            {
                scroll_state.has_non_viewport_wheel_scroll_target_candidate = true;
            }
        }
        Some(RegisteredScrollLikeNode::Scroll)
    }
}

pub(crate) fn rebuild_scroll_state_from_tree(
    layout_arena: &impl PaintableRowsRead,
    tree: &mut VisualContextTree,
    tree_inputs: &FfiVisualContextTreeInputs,
    delta: &mut VisualContextTreeDelta,
) -> ScrollState {
    let mut scroll_state = ScrollState::default();
    for node_index in tree.spatial_dependency_order() {
        let node_index = SpatialNodeIndex(node_index);
        if register_scroll_like_node(layout_arena, tree, &mut scroll_state, node_index)
            != Some(RegisteredScrollLikeNode::Sticky)
        {
            continue;
        }
        let index = node_index.0 as usize;
        let SpatialData::Sticky(sticky) = &tree.spatial_nodes[index].data else {
            unreachable!("a registered sticky node keeps its payload kind");
        };
        let refreshed = SpatialData::Sticky(compute_sticky_data(
            layout_arena,
            &scroll_state,
            sticky.state_slot,
            tree_inputs,
        ));
        if !super::shape::spatial_payloads_are_equal(&tree.spatial_nodes[index].data, &refreshed) {
            delta.note_patched_spatial(node_index.0);
        }
        tree.spatial_nodes[index].data = refreshed;
    }
    scroll_state
}

pub(crate) fn box_owns_geometry_dependent_nodes(
    layout_arena: &impl PaintableRowsRead,
    tree: &VisualContextTree,
    slot: NodeSlotId,
    handles: &BoxVisualContextNodeHandles,
) -> bool {
    if layout_arena.paintable_side_data(slot).svg_filter_bounds.get().is_some() {
        return true;
    }
    let spatial_is_geometry_dependent = handles.spatial.iter().any(|index| {
        matches!(
            tree.spatial_nodes[index.0 as usize].data,
            SpatialData::Transform(_) | SpatialData::Perspective(_) | SpatialData::Sticky(_)
        )
    });
    let effects_filter_is_resolved_by_the_host_against_geometry = layout_arena
        .node_style_if_live(slot)
        .is_some_and(|style| crate::painting::filter_bytes::contains_url(&style.effects().filter));
    let frames_are_geometry_dependent = handles.frame_handles().any(|index| {
        let node = &tree.frame_nodes[index.0 as usize];
        match &node.data {
            FrameData::Clip(_) | FrameData::ClipPath(_) | FrameData::Mask(_) => true,
            FrameData::Effects(effects) => {
                effects.filter.is_some() && effects_filter_is_resolved_by_the_host_against_geometry
            }
            FrameData::Dead => false,
        }
    });
    spatial_is_geometry_dependent || frames_are_geometry_dependent
}

struct PendingBox {
    slot: NodeSlotId,
    parent: Option<NodeSlotId>,
    input: DescendantVisualContexts,
    cascade: ChildCascade,
}

struct DeferredAnchorPositionedBox {
    pending: PendingBox,
    anchor: NodeSlotId,
}

struct WalkAnchorScrollShiftResolver<'a, Arena> {
    layout_arena: &'a Arena,
    callbacks: &'a FfiVisualContextHostCallbacks,
    scroll_state: &'a ScrollState,
    assignments: &'a [PaintableVisualContextAssignment],
    assignment_index_by_slot: &'a HashMap<NodeSlotId, usize>,
    may_have_default_scroll_shift_anchor: bool,
}

impl<Arena: PaintableRowsRead> AnchorScrollShiftResolver for WalkAnchorScrollShiftResolver<'_, Arena> {
    fn default_scroll_shift_anchor(&self, slot: NodeSlotId) -> NodeSlotId {
        if !self.may_have_default_scroll_shift_anchor {
            return NodeSlotId::INVALID;
        }
        default_scroll_shift_anchor_of(self.layout_arena, self.callbacks, slot)
    }

    fn enclosing_scroll_node_index(&self, slot: NodeSlotId) -> SpatialNodeIndex {
        // A box outside this pass's fresh assignments is only reachable through a malformed
        // anchor chain; its committed index may name a node the scaffold never registered, so
        // it contributes no scroll shift instead of a stale lookup.
        match self.assignment_index_by_slot.get(&slot) {
            Some(&index) => self.assignments[index].enclosing_scroll_node_index,
            None => VISUAL_VIEWPORT_NODE_INDEX,
        }
    }

    fn scroll_state(&self) -> &ScrollState {
        self.scroll_state
    }
}

fn default_scroll_shift_anchor_of(
    layout_arena: &impl PaintableRowsRead,
    callbacks: &FfiVisualContextHostCallbacks,
    slot: NodeSlotId,
) -> NodeSlotId {
    callbacks.default_scroll_shift_anchor(layout_arena.shell_if_live(slot))
}

fn anchor_is_awaiting_build(
    layout_arena: &impl PaintableRowsRead,
    anchor_node: NodeSlotId,
    awaiting: &HashSet<NodeSlotId>,
) -> bool {
    let mut paintable = layout_arena
        .paintable_row_is_populated(anchor_node)
        .then_some(anchor_node);
    while let Some(current) = paintable {
        if awaiting.contains(&current) {
            return true;
        }
        paintable = paint_order::paint_parent(layout_arena, current);
    }
    false
}

fn take_next_deferred_anchor_positioned(
    layout_arena: &impl PaintableRowsRead,
    deferred: &mut Vec<DeferredAnchorPositionedBox>,
    awaiting: &mut HashSet<NodeSlotId>,
) -> Option<PendingBox> {
    if deferred.is_empty() {
        return None;
    }
    // Cyclic or otherwise malformed anchor chains can leave every entry waiting on another;
    // the first entry is then built in queue order.
    let ready_position = deferred
        .iter()
        .position(|entry| !anchor_is_awaiting_build(layout_arena, entry.anchor, awaiting))
        .unwrap_or(0);
    let entry = deferred.remove(ready_position);
    awaiting.remove(&entry.pending.slot);
    Some(entry.pending)
}

pub(crate) fn update_visual_context_tree<Arena: PaintableRowsRead>(
    layout_arena: &Arena,
    callbacks: &FfiVisualContextHostCallbacks,
    viewport: NodeSlotId,
    tree_inputs: FfiVisualContextTreeInputs,
    root_background_source: FfiRootBackgroundSource,
    scope: VisualContextUpdateScope,
    state: &mut VisualContextState,
) -> IncrementalUpdateResult {
    let plan = if scope.rebuilds_every_box() {
        WorkPlan::default()
    } else {
        match expand_dirty_entries(layout_arena, &state.dirty_boxes, root_background_source) {
            Ok(plan) => plan,
            Err(reason) => return IncrementalUpdateResult::NeedsFullBuild(reason),
        }
    };
    let Some(tree) = state.tree.as_mut() else {
        return IncrementalUpdateResult::NeedsFullBuild(VisualContextGlobalRebuildReason::FirstBuild);
    };
    let mut delta = VisualContextTreeDelta::default();
    debug_assert!(
        scope != VisualContextUpdateScope::FreshTree || state.dirty_boxes.removed.is_empty(),
        "removed handles name nodes of the discarded tree"
    );
    tombstone_removed_blocks(Rc::make_mut(tree), &state.dirty_boxes, &mut delta);

    let environment = BoxBuildEnvironment {
        layout_arena,
        callbacks,
        pixel_ratio: tree_inputs.device_pixels_per_css_pixel,
        tree_inputs,
        root_background_source,
    };
    let viewport_output = layout_arena
        .paintable_visual_context_record(viewport)
        .map(|record| record.output_for_descendants);
    let Some(viewport_output) = viewport_output else {
        return IncrementalUpdateResult::NeedsFullBuild(VisualContextGlobalRebuildReason::FirstBuild);
    };
    let mut scaffold_scroll_state = scope.rebuilds_every_box().then(ScrollState::default);
    if let Some(scroll_state) = scaffold_scroll_state.as_mut() {
        let tree = Rc::make_mut(tree);
        tree.set_visual_viewport_transform(super::node_values::visual_viewport_transform_data(&tree_inputs));
        register_scroll_like_node(layout_arena, tree, scroll_state, viewport_output.normal.spatial);
    }
    let every_box_capacity = if scope.rebuilds_every_box() {
        layout_arena.paintable_row_count()
    } else {
        0
    };
    let mut assignments: Vec<PaintableVisualContextAssignment> = Vec::with_capacity(every_box_capacity);
    let mut assignment_index_by_slot: HashMap<NodeSlotId, usize> = HashMap::with_capacity(every_box_capacity);
    let mut mask_node_owners_changed = state
        .dirty_boxes
        .removed
        .iter()
        .any(|removed| state.paintables_with_mask_nodes.contains(&removed.slot));
    let mut stack: Vec<PendingBox> = Vec::new();
    let push_children = |stack: &mut Vec<PendingBox>,
                         parent: NodeSlotId,
                         input_for_children: DescendantVisualContexts,
                         cascade: ChildCascade,
                         visit_every_child: bool| {
        let pushed_children_begin = stack.len();
        paint_order::for_each_paint_child(layout_arena, parent, |child| {
            if visit_every_child || plan.work.contains_key(&child) || plan.ancestors_of_work.contains(&child) {
                stack.push(PendingBox {
                    slot: child,
                    parent: Some(parent),
                    input: input_for_children,
                    cascade,
                });
            }
        });
        stack[pushed_children_begin..].reverse();
    };
    push_children(
        &mut stack,
        viewport,
        viewport_output,
        ChildCascade::default(),
        scope.rebuilds_every_box() || plan.revalidate_children_of.contains(&viewport),
    );

    let defers_anchor_positioned = scope.rebuilds_every_box() && tree_inputs.may_have_default_scroll_shift_anchor;
    let mut deferred_anchor_positioned: Vec<DeferredAnchorPositionedBox> = Vec::new();
    let mut deferred_awaiting_build: HashSet<NodeSlotId> = HashSet::new();
    loop {
        let (pending, may_defer_this_box) = match stack.pop() {
            Some(pending) => (pending, defers_anchor_positioned),
            None => match take_next_deferred_anchor_positioned(
                layout_arena,
                &mut deferred_anchor_positioned,
                &mut deferred_awaiting_build,
            ) {
                Some(pending) => (pending, false),
                None => break,
            },
        };
        let slot = pending.slot;
        if may_defer_this_box {
            let anchor = default_scroll_shift_anchor_of(layout_arena, callbacks, slot);
            if !anchor.is_invalid() {
                deferred_awaiting_build.insert(slot);
                deferred_anchor_positioned.push(DeferredAnchorPositionedBox { pending, anchor });
                continue;
            }
        }
        let parent = pending
            .parent
            .expect("every pending box below the viewport has a paint parent");
        let input = pending.input;
        let work_bits = plan.work.get(&slot).copied();
        let (rebuild, subtree_may_own_geometry_dependent_nodes) = if scope.rebuilds_every_box() {
            (true, false)
        } else {
            let record = layout_arena.paintable_visual_context_record(slot);
            let record = record.as_deref();
            let rebuild = work_bits.is_some()
                || record.is_none_or(|record| record.inherited_input != input)
                || (pending.cascade.geometry_walk && record.is_some_and(|record| record.owns_geometry_dependent_nodes));
            (
                rebuild,
                record.is_some_and(|record| record.subtree_may_own_geometry_dependent_nodes),
            )
        };
        let (child_cascade, output_for_children) = if rebuild {
            let existing_record = layout_arena.paintable_visual_context_record(slot);
            let record_existed = existing_record.is_some();
            let previous_output = existing_record.as_ref().map(|record| record.output_for_descendants);
            let previous_has_mask_nodes = existing_record.as_ref().is_some_and(|record| record.has_mask_nodes);
            let may_be_root_element = parent == viewport;
            let tree = Rc::make_mut(state.tree.as_mut().expect("the tree exists throughout the pass"));
            let mut scratch = BoxNodeScratch::new(tree);
            let output = {
                let anchor_scroll_shift_resolver =
                    scaffold_scroll_state
                        .as_ref()
                        .map(|scroll_state| WalkAnchorScrollShiftResolver {
                            layout_arena,
                            callbacks,
                            scroll_state,
                            assignments: &assignments,
                            assignment_index_by_slot: &assignment_index_by_slot,
                            may_have_default_scroll_shift_anchor: tree_inputs.may_have_default_scroll_shift_anchor,
                        });
                build_box_visual_context_nodes(
                    &environment,
                    &mut scratch,
                    slot,
                    input,
                    may_be_root_element,
                    anchor_scroll_shift_resolver
                        .as_ref()
                        .map(|resolver| resolver as &dyn AnchorScrollShiftResolver),
                )
            };
            let (scratch_spatial, scratch_frames) = scratch.into_nodes();
            let existing_handles = existing_record.as_ref().map(|record| &record.node_handles);
            let placement = plan_box_node_placement(
                tree,
                existing_handles,
                &output.assignment.record.node_handles,
                &mut delta,
            );
            let reconcile = write_box_nodes(
                tree,
                scratch_spatial,
                scratch_frames,
                &placement,
                existing_handles,
                &mut delta,
            );
            if let Some(scroll_state) = scaffold_scroll_state.as_mut() {
                for handle in &placement.spatial {
                    register_scroll_like_node(layout_arena, tree, scroll_state, *handle);
                }
            }
            let mut assignment = output.assignment;
            assignment.accumulated_visual_context = placement.remap_context(assignment.accumulated_visual_context);
            assignment.accumulated_visual_context_for_descendants =
                placement.remap_context(assignment.accumulated_visual_context_for_descendants);
            assignment.fixed_background_visual_context =
                placement.remap_context(assignment.fixed_background_visual_context);
            assignment.enclosing_scroll_node_index = placement.remap_spatial(assignment.enclosing_scroll_node_index);
            assignment.own_scroll_node_index = placement.remap_spatial(assignment.own_scroll_node_index);
            let new_output = remap_descendant_contexts(&placement, output.descendant_contexts);
            assignment.record.inherited_input = input;
            assignment.record.output_for_descendants = new_output;
            assignment.record.node_handles = placement.into_node_handles();
            assignment.record.owns_geometry_dependent_nodes =
                box_owns_geometry_dependent_nodes(layout_arena, tree, slot, &assignment.record.node_handles);
            assignment.record.subtree_may_own_geometry_dependent_nodes =
                assignment.record.owns_geometry_dependent_nodes || subtree_may_own_geometry_dependent_nodes;

            let previous_contexts = record_existed.then(|| {
                let data = layout_arena.paintable_data(slot);
                (
                    data.accumulated_visual_context,
                    data.accumulated_visual_context_for_descendants,
                )
            });
            let contexts_changed = previous_contexts
                != Some((
                    assignment.accumulated_visual_context,
                    assignment.accumulated_visual_context_for_descendants,
                ));
            if reconcile.shape_changed || !record_existed {
                layout_arena.invalidate_paint_cache(slot);
            } else if contexts_changed {
                layout_arena
                    .paintable_rows()
                    .mark_descendant_subtree_caches_dirty_along_paint_chain(slot);
            }
            if previous_has_mask_nodes != assignment.record.has_mask_nodes {
                mask_node_owners_changed = true;
            }
            drop(existing_record);
            if assignment.record.owns_geometry_dependent_nodes && !subtree_may_own_geometry_dependent_nodes {
                let mut ancestor = paint_order::paint_parent(layout_arena, slot);
                while let Some(current) = ancestor {
                    let ancestor_was_flagged = if let Some(&index) = assignment_index_by_slot.get(&current) {
                        let flagged = assignments[index].record.subtree_may_own_geometry_dependent_nodes;
                        assignments[index].record.subtree_may_own_geometry_dependent_nodes = true;
                        flagged
                    } else {
                        layout_arena
                            .mark_paintable_subtree_may_own_geometry_dependent_nodes(current)
                            .unwrap_or(true)
                    };
                    if ancestor_was_flagged {
                        break;
                    }
                    ancestor = paint_order::paint_parent(layout_arena, current);
                }
            }
            assignment_index_by_slot.insert(slot, assignments.len());
            assignments.push(assignment);
            let cascade = ChildCascade {
                input_changed: previous_output != Some(new_output),
                geometry_walk: pending.cascade.geometry_walk || work_bits.is_some_and(|bits| bits.moves_descendants()),
            };
            (cascade, Some(new_output))
        } else {
            let cascade = ChildCascade {
                input_changed: false,
                geometry_walk: pending.cascade.geometry_walk && subtree_may_own_geometry_dependent_nodes,
            };
            (cascade, None)
        };
        let revalidate_children = plan.revalidate_children_of.contains(&slot);
        let visit_every_child = scope.rebuilds_every_box() || child_cascade.visits_every_child() || revalidate_children;
        if visit_every_child || plan.ancestors_of_work.contains(&slot) {
            let output_for_children = output_for_children.unwrap_or_else(|| {
                layout_arena
                    .paintable_visual_context_record(slot)
                    .expect("a box that was not rebuilt keeps its record")
                    .output_for_descendants
            });
            push_children(&mut stack, slot, output_for_children, child_cascade, visit_every_child);
        }
    }

    let tree = Rc::make_mut(state.tree.as_mut().expect("the tree exists throughout the pass"));
    let scroll_state = rebuild_scroll_state_from_tree(layout_arena, tree, &tree_inputs, &mut delta);
    delta.finish();
    tree.debug_assert_slot_accounting();
    if delta.structural_epoch_changed {
        tree.structural_epoch = allocate_structural_epoch();
    }
    state.scroll_state = scroll_state;
    state.scroll_state_snapshot.clear();
    state.needs_to_refresh_scroll_state = true;
    IncrementalUpdateResult::Applied(IncrementalUpdateOutcome {
        delta,
        assignments,
        mask_node_owners_changed,
    })
}

#[cfg(debug_assertions)]
pub(crate) fn debug_assert_every_live_node_is_owned(
    layout_arena: &impl PaintableRowsRead,
    tree: &VisualContextTree,
    viewport: NodeSlotId,
) {
    let mut spatial_is_owned = vec![false; tree.spatial_nodes.len()];
    let mut frame_is_owned = vec![false; tree.frame_nodes.len()];
    spatial_is_owned[VISUAL_VIEWPORT_NODE_INDEX.0 as usize] = true;
    let viewport_scroll_node = layout_arena.paintable_data(viewport).own_scroll_node_index;
    if (viewport_scroll_node.0 as usize) < spatial_is_owned.len() {
        spatial_is_owned[viewport_scroll_node.0 as usize] = true;
    }
    if let Some(root_isolation_frame) = tree.root_isolation_frame {
        frame_is_owned[root_isolation_frame.0 as usize] = true;
    }
    paint_order::for_each_in_paint_subtree(layout_arena, viewport, |slot| {
        let Some(record) = layout_arena.paintable_visual_context_record(slot) else {
            return;
        };
        for index in &record.node_handles.spatial {
            assert!(
                !spatial_is_owned[index.0 as usize],
                "spatial node {} is claimed twice",
                index.0
            );
            spatial_is_owned[index.0 as usize] = true;
        }
        for index in record.node_handles.frame_handles() {
            assert!(
                !frame_is_owned[index.0 as usize],
                "frame node {} is claimed twice",
                index.0
            );
            frame_is_owned[index.0 as usize] = true;
        }
    });
    for (index, node) in tree.spatial_nodes.iter().enumerate() {
        assert!(
            !node.data.is_live() || spatial_is_owned[index],
            "live spatial node {index} has no owner"
        );
    }
    for (index, node) in tree.frame_nodes.iter().enumerate() {
        assert!(
            !node.data.is_live() || frame_is_owned[index],
            "live frame node {index} has no owner"
        );
    }
}

#[cfg(not(debug_assertions))]
pub(crate) fn debug_assert_every_live_node_is_owned(
    _layout_arena: &impl PaintableRowsRead,
    _tree: &VisualContextTree,
    _viewport: NodeSlotId,
) {
}

fn remap_descendant_contexts(
    placement: &super::reconcile::BoxNodePlacement,
    contexts: DescendantVisualContexts,
) -> DescendantVisualContexts {
    let remap_scroll_nodes = |nodes: NearestScrollNodeIndices| NearestScrollNodeIndices {
        stopping_at_fixed_position_ancestors: placement.remap_spatial(nodes.stopping_at_fixed_position_ancestors),
        continuing_through_fixed_position_ancestors: placement
            .remap_spatial(nodes.continuing_through_fixed_position_ancestors),
    };
    DescendantVisualContexts {
        normal: placement.remap_context(contexts.normal),
        absolute_position: placement.remap_context(contexts.absolute_position),
        fixed_position: placement.remap_context(contexts.fixed_position),
        normal_nearest_scroll_nodes: remap_scroll_nodes(contexts.normal_nearest_scroll_nodes),
        absolute_position_nearest_scroll_nodes: remap_scroll_nodes(contexts.absolute_position_nearest_scroll_nodes),
        fixed_position_nearest_scroll_nodes: remap_scroll_nodes(contexts.fixed_position_nearest_scroll_nodes),
        normal_plane_root: placement.remap_spatial(contexts.normal_plane_root),
        absolute_position_plane_root: placement.remap_spatial(contexts.absolute_position_plane_root),
        fixed_position_plane_root: placement.remap_spatial(contexts.fixed_position_plane_root),
        flattens_inherited_transform: contexts.flattens_inherited_transform,
        sorting_context_root: contexts
            .sorting_context_root
            .map(|index| placement.remap_spatial(index)),
    }
}
