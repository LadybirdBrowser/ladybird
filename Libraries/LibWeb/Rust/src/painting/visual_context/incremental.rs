/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::box_build::{BoxBuildEnvironment, PaintableVisualContextAssignment, build_box_visual_context_nodes};
use super::delta::VisualContextTreeDelta;
use super::dirty::{BoxDirtyBits, VisualContextBoxDirtyKind, VisualContextDirtySet, VisualContextGlobalRebuildReason};
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
        if layout_arena.node_kind_if_live(*slot) == Some(NodeKind::LegendBox)
            && let Some(fieldset) = paint_order::paint_parent(layout_arena, *slot)
        {
            work.entry(fieldset)
                .or_default()
                .insert(VisualContextBoxDirtyKind::RecommittedInPlace);
        }
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

pub(crate) fn rebuild_scroll_state_from_tree(
    layout_arena: &impl PaintableRowsRead,
    tree: &mut VisualContextTree,
    tree_inputs: &FfiVisualContextTreeInputs,
    delta: &mut VisualContextTreeDelta,
) -> ScrollState {
    let mut scroll_state = ScrollState::default();
    for node_index in tree.spatial_dependency_order() {
        let index = node_index as usize;
        let node_index = SpatialNodeIndex(node_index);
        let (is_sticky, owner_paintable, registry_parent_node) = match &tree.spatial_nodes[index].data {
            SpatialData::Scroll(scroll) => (false, scroll.owner_paintable, scroll.registry_parent_node),
            SpatialData::Sticky(sticky) => (true, sticky.owner_paintable, sticky.registry_parent_node),
            _ => continue,
        };
        let parent_slot = tree.scroll_state_slot_for_node(registry_parent_node);
        if is_sticky {
            let slot = scroll_state.register_sticky_node(node_index, owner_paintable, parent_slot);
            let refreshed = SpatialData::Sticky(compute_sticky_data(layout_arena, &scroll_state, slot, tree_inputs));
            if !super::shape::spatial_payloads_are_equal(&tree.spatial_nodes[index].data, &refreshed) {
                delta.note_patched_spatial(node_index.0);
            }
            tree.spatial_nodes[index].data = refreshed;
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
        }
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
        node.role != FrameRole::Structural
            || match &node.data {
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
    cascade: ChildCascade,
}

pub(crate) fn update_visual_context_tree_incrementally<Arena: PaintableRowsRead>(
    layout_arena: &Arena,
    callbacks: &FfiVisualContextHostCallbacks,
    viewport: NodeSlotId,
    tree_inputs: FfiVisualContextTreeInputs,
    root_background_source: FfiRootBackgroundSource,
    state: &mut VisualContextState,
) -> IncrementalUpdateResult {
    let plan = match expand_dirty_entries(layout_arena, &state.dirty_boxes, root_background_source) {
        Ok(plan) => plan,
        Err(reason) => return IncrementalUpdateResult::NeedsFullBuild(reason),
    };
    let Some(tree) = state.tree.as_mut() else {
        return IncrementalUpdateResult::NeedsFullBuild(VisualContextGlobalRebuildReason::FirstBuild);
    };
    let mut delta = VisualContextTreeDelta::default();
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
    let mut fresh_outputs: HashMap<NodeSlotId, DescendantVisualContexts> = HashMap::new();
    fresh_outputs.insert(viewport, viewport_output);
    let mut assignments: Vec<PaintableVisualContextAssignment> = Vec::new();
    let mut assignment_index_by_slot: HashMap<NodeSlotId, usize> = HashMap::new();
    let mut mask_node_owners_changed = state
        .dirty_boxes
        .removed
        .iter()
        .any(|removed| state.paintables_with_mask_nodes.contains(&removed.slot));
    let mut stack: Vec<PendingBox> = Vec::new();
    let push_children =
        |stack: &mut Vec<PendingBox>, parent: NodeSlotId, cascade: ChildCascade, visit_every_child: bool| {
            let mut children = Vec::new();
            paint_order::for_each_paint_child(layout_arena, parent, |child| {
                if visit_every_child || plan.work.contains_key(&child) || plan.ancestors_of_work.contains(&child) {
                    children.push(child);
                }
            });
            for child in children.into_iter().rev() {
                stack.push(PendingBox {
                    slot: child,
                    parent: Some(parent),
                    cascade,
                });
            }
        };
    push_children(
        &mut stack,
        viewport,
        ChildCascade::default(),
        plan.revalidate_children_of.contains(&viewport),
    );

    while let Some(pending) = stack.pop() {
        let slot = pending.slot;
        let parent = pending
            .parent
            .expect("every pending box below the viewport has a paint parent");
        let input = fresh_outputs.get(&parent).copied().unwrap_or_else(|| {
            layout_arena
                .paintable_visual_context_record(parent)
                .expect("a paint parent that was not rebuilt keeps its record")
                .output_for_descendants
        });
        let work_bits = plan.work.get(&slot).copied();
        let (rebuild, subtree_may_own_geometry_dependent_nodes) = {
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
        let child_cascade = if rebuild {
            let existing_record = layout_arena.cloned_paintable_visual_context_record(slot);
            let may_be_root_element = parent == viewport;
            let tree = Rc::make_mut(state.tree.as_mut().expect("the tree exists throughout the pass"));
            let mut scratch = BoxNodeScratch::new(tree);
            let output =
                build_box_visual_context_nodes(&environment, &mut scratch, slot, input, may_be_root_element, None);
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

            let previous_output = existing_record.as_ref().map(|record| record.output_for_descendants);
            let previous_contexts = existing_record.as_ref().map(|_| {
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
            if reconcile.shape_changed || existing_record.is_none() {
                layout_arena.invalidate_paint_cache(slot);
            } else if contexts_changed {
                layout_arena
                    .paintable_rows()
                    .mark_descendant_subtree_caches_dirty_along_paint_chain(slot);
            }
            if existing_record.as_ref().is_some_and(|record| record.has_mask_nodes) != assignment.record.has_mask_nodes
            {
                mask_node_owners_changed = true;
            }
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
            fresh_outputs.insert(slot, new_output);
            assignment_index_by_slot.insert(slot, assignments.len());
            assignments.push(assignment);
            ChildCascade {
                input_changed: previous_output != Some(new_output),
                geometry_walk: pending.cascade.geometry_walk || work_bits.is_some_and(|bits| bits.moves_descendants()),
            }
        } else {
            ChildCascade {
                input_changed: false,
                geometry_walk: pending.cascade.geometry_walk && subtree_may_own_geometry_dependent_nodes,
            }
        };
        let revalidate_children = plan.revalidate_children_of.contains(&slot);
        if child_cascade.visits_every_child() || revalidate_children || plan.ancestors_of_work.contains(&slot) {
            push_children(
                &mut stack,
                slot,
                child_cascade,
                child_cascade.visits_every_child() || revalidate_children,
            );
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
