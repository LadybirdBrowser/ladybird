/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::LayoutNodeArena;
use crate::layout::formatting_context::{FfiFormattingContextType, formatting_context_type_created_by_node_data};
use crate::layout::node_data::{NodeFlag, NodeKind, NodeSlotId};
use crate::layout::node_facts;
use std::ffi::c_void;

#[repr(C)]
pub struct FfiLayoutTreeUpdateClassification {
    pub marks_partial_relayout_boundary_self_only: bool,
    pub nearest_non_anonymous_ancestor_when_parent_is_anonymous: NodeSlotId,
}

impl LayoutNodeArena {
    /// Classifies how a layout tree update on this node reaches layout: a detached node
    /// escapes partial relayout; a structural self-rebuild on a partial relayout boundary
    /// marks the boundary alone, and a child-list mutation cannot change the boundary's own
    /// box kind, so replacing its box in place cannot require restructuring the surrounding
    /// anonymous siblings; otherwise a node under an anonymous parent escalates the rebuild
    /// to the nearest non-anonymous ancestor.
    pub(crate) fn classify_layout_tree_update(
        &self,
        node: NodeSlotId,
        reason_is_structural_boundary_self_rebuild: bool,
    ) -> FfiLayoutTreeUpdateClassification {
        let data = self.data(node);
        let (kind, parent) = (data.kind.get(), data.parent.get());

        // A dirty DOM node whose box is outside the layout tree cannot attribute its update to
        // any boundary.
        if parent.is_invalid() && kind != NodeKind::Viewport {
            self.record_partial_relayout_escape();
        }

        let marks_boundary_self_only = node_facts::kind_is_box(kind)
            && reason_is_structural_boundary_self_rebuild
            && self.node_is_partial_relayout_boundary(node);

        let mut nearest_non_anonymous_ancestor = NodeSlotId::INVALID;
        if !marks_boundary_self_only && !parent.is_invalid() {
            let mut ancestor = parent;
            let mut ancestor_is_first = true;
            while !ancestor.is_invalid() {
                let (ancestor_flags, ancestor_parent) = {
                    let ancestor_data = self.data(ancestor);
                    (ancestor_data.flags.get(), ancestor_data.parent.get())
                };
                if ancestor_flags & NodeFlag::Anonymous as u32 == 0 {
                    if !ancestor_is_first {
                        nearest_non_anonymous_ancestor = ancestor;
                    }
                    break;
                }
                ancestor = ancestor_parent;
                ancestor_is_first = false;
            }
        }

        FfiLayoutTreeUpdateClassification {
            marks_partial_relayout_boundary_self_only: marks_boundary_self_only,
            nearest_non_anonymous_ancestor_when_parent_is_anonymous: nearest_non_anonymous_ancestor,
        }
    }

    fn commit_splice_position_is_derivable_from_layout_ancestors(&self, node: NodeSlotId) -> bool {
        let paintable_rows = self.paintable_rows();
        let mut ancestor = self.data(node).parent.get();
        while !ancestor.is_invalid() {
            if paintable_rows.paintable_row_is_populated(ancestor) {
                return true;
            }
            let ancestor_data = self.data(ancestor);
            if !node_facts::node_is_fragmented_inline(ancestor_data, node_facts::node_style_view(ancestor_data)) {
                return false;
            }
            ancestor = ancestor_data.parent.get();
        }
        false
    }

    pub(crate) fn node_is_partial_relayout_boundary(&self, node: NodeSlotId) -> bool {
        let data = self.data(node);

        // An absolutely or fixed positioned descendant whose containing block is outside this
        // box's subtree is laid out by a formatting context outside it, which makes subtree
        // isolation impossible for any kind of boundary.
        if node_facts::has_flag(data, NodeFlag::AbsposDescendantEscapes) {
            return false;
        }

        if !self.paintable_rows().paintable_row_is_populated(node)
            && !self.commit_splice_position_is_derivable_from_layout_ancestors(node)
        {
            return false;
        }

        let style = node_facts::node_style_view(data);
        let style_is_absolutely_positioned = style.is_some_and(|style| style.is_absolutely_positioned());

        // An in-flow SVG viewport's used size is determined solely by its own attributes and outer
        // context, never by its children, so its size and position from the previous layout can be
        // reused - provided a commit has actually saved them; its content lays out in the viewport's
        // own user units, so a nested <svg> is just as reproducible from its own root as the
        // outermost one. An absolutely positioned SVG root's placement is not frozen, so it must
        // qualify through the saved-inputs replay path below instead.
        if data.kind.get() == NodeKind::SVGSVGBox && !style_is_absolutely_positioned {
            return node_facts::has_flag(data, NodeFlag::HasCommittedFragmentLink);
        }

        if !style_is_absolutely_positioned {
            return false;
        }
        if node_facts::has_flag(data, NodeFlag::Anonymous) {
            return false;
        }
        if node_facts::has_flag(data, NodeFlag::IsDocumentElement) {
            return false;
        }
        if !node_facts::has_flag(data, NodeFlag::HasSavedAbsposLayoutInputs) {
            return false;
        }

        // Only a full layout pass resolves anchor() functions in the inset properties to plain
        // values; a replay from saved inputs cannot.
        if node_facts::has_flag(data, NodeFlag::InsetsUseAnchorFunctions) {
            return false;
        }

        // NOTE: Content-dependent sizing (shrink-to-fit, intrinsic constraints, aspect-ratio) does
        //       not disqualify a boundary: replay re-solves the boundary's own size, and a resized
        //       boundary triggers ancestor scrollable overflow recomputation after commit.

        let parent_style = (!data.parent.get().is_invalid())
            .then(|| self.style_payloads(data.parent.get()))
            .flatten()
            .map(|payloads| crate::css::computed_value_views::ComputedValuesView::new(&payloads.groups));
        matches!(
            formatting_context_type_created_by_node_data(data, style, parent_style),
            Some(
                FfiFormattingContextType::Block
                    | FfiFormattingContextType::Flex
                    | FfiFormattingContextType::Grid
                    | FfiFormattingContextType::Svg
            )
        )
    }

    pub(crate) fn register_partial_relayout_boundary_root(&self, node: NodeSlotId) {
        let kind = self.data(node).kind.get();
        assert!(node_facts::kind_is_box(kind));
        let mut roots = self.partial_relayout_boundary_roots.borrow_mut();
        roots.retain(|candidate| self.slot_is_live(*candidate));
        if roots.contains(&node) {
            return;
        }
        roots.push(node);
    }

    /// Counts stale entries for freed nodes on purpose: the C++ side treats a nonempty
    /// root set as "layout is not up to date", and a freed boundary still attributes a
    /// pending update, exactly as a nulled-out weak pointer did.
    pub(crate) fn has_partial_relayout_boundary_roots(&self) -> bool {
        !self.partial_relayout_boundary_roots.borrow().is_empty()
    }

    pub(crate) fn take_partial_relayout_boundary_roots(&self) -> Vec<NodeSlotId> {
        std::mem::take(&mut *self.partial_relayout_boundary_roots.borrow_mut())
    }

    pub(crate) fn record_partial_relayout_escape(&self) {
        self.pending_updates_escape_partial_relayout.set(true);
    }

    pub(crate) fn clear_partial_relayout_escape(&self) {
        self.pending_updates_escape_partial_relayout.set(false);
    }

    fn node_needs_layout_update(&self, node: NodeSlotId) -> bool {
        node_facts::has_flag(self.data(node), NodeFlag::NeedsLayoutUpdate)
    }

    /// Whether the pending update may be satisfied by re-laying out only registered boundary
    /// subtrees. Decided before the layout tree build, so an ineligible update takes the full
    /// layout path's build instead. `root` may be the invalid slot when no tree exists yet.
    pub(crate) fn partial_relayout_may_be_attempted(
        &self,
        root: NodeSlotId,
        registered_root_slots: &[NodeSlotId],
        facts: FfiPartialRelayoutHostFacts,
    ) -> bool {
        !(root.is_invalid()
            || facts.document_needs_full_layout_tree_update
            || self.pending_updates_escape_partial_relayout.get()
            || registered_root_slots.is_empty()
            || self.node_needs_layout_update(root)
            || facts.container_query_evaluation_is_pending
            || facts.should_collect_devtools_layout_data
            || facts.any_anchor_names_are_registered)
    }

    /// Selects the boundary subtrees to relay out after the layout tree build. Consumes the
    /// escape bit either way: a refusal here is followed by a full layout pass in the same
    /// update, which re-derives every fact the bit stands in for.
    pub(crate) fn plan_partial_relayout(
        &self,
        root: NodeSlotId,
        registered_root_slots: &[NodeSlotId],
        rebuilt_subtree_root_slots: &[NodeSlotId],
        layout_tree_update_escaped_rebuild_roots: bool,
    ) -> Option<Vec<NodeSlotId>> {
        let pending_updates_escaped =
            self.pending_updates_escape_partial_relayout.replace(false) || layout_tree_update_escaped_rebuild_roots;
        if self.node_needs_layout_update(root) || pending_updates_escaped {
            return None;
        }
        self.collect_partial_relayout_roots(registered_root_slots, rebuilt_subtree_root_slots)
    }

    fn nearest_inclusive_partial_relayout_boundary(&self, node: NodeSlotId) -> Option<NodeSlotId> {
        let (node_kind, mut ancestor) = {
            let data = self.data(node);
            (data.kind.get(), data.parent.get())
        };
        if node_facts::kind_is_box(node_kind) && self.node_is_partial_relayout_boundary(node) {
            return Some(node);
        }
        while !ancestor.is_invalid() {
            let (ancestor_kind, ancestor_parent) = {
                let data = self.data(ancestor);
                (data.kind.get(), data.parent.get())
            };
            if node_facts::kind_is_box(ancestor_kind) && self.node_is_partial_relayout_boundary(ancestor) {
                return Some(ancestor);
            }
            ancestor = ancestor_parent;
        }
        None
    }

    /// Collects the live boundary set from the post-build tree: registered boundaries that
    /// survived the build, plus the nearest boundary containing each rebuilt subtree - which
    /// re-discovers a boundary whose own box the build replaced, since the saved layout inputs
    /// carried over to the replacement. Returns None when any boundary disqualifies partial
    /// relayout, or when no boundary is left to relay out.
    pub(crate) fn collect_partial_relayout_roots(
        &self,
        registered_root_slots: &[NodeSlotId],
        rebuilt_subtree_root_slots: &[NodeSlotId],
    ) -> Option<Vec<NodeSlotId>> {
        let mut collected_boundaries: crate::css::style::fast_hash::FastSet<NodeSlotId> = Default::default();
        let mut partial_relayout_roots: Vec<NodeSlotId> = Vec::new();
        let mut collect_boundary = |boundary: NodeSlotId, boundary_box_was_replaced: bool| -> bool {
            if !collected_boundaries.insert(boundary) {
                return true;
            }

            // A replaced box applies the saved-inputs validity check unconditionally: the change
            // that drove the replacement cannot be classified anymore.
            let boundary_data = self.data(boundary);
            let saved_inputs_may_be_style_stale =
                node_facts::has_flag(boundary_data, NodeFlag::NeedsOwnGeometryUpdate) || boundary_box_was_replaced;
            let boundary_is_absolutely_positioned =
                node_facts::node_style_view(boundary_data).is_some_and(|style| style.is_absolutely_positioned());
            if saved_inputs_may_be_style_stale
                && boundary_is_absolutely_positioned
                && !self.node_can_replay_saved_abspos_layout_inputs_after_style_change(boundary)
            {
                return false;
            }

            partial_relayout_roots.push(boundary);
            true
        };

        for &slot in registered_root_slots {
            // A boundary that did not survive the build was either replaced (re-discovered
            // through the rebuilt subtree roots below) or removed together with the dirt
            // inside it (the removal dirtied its parent, whose own marking covers the
            // mutation). Slot generations make the stale slot ids of such boundaries
            // resolve to no live slot.
            if !self.slot_is_live(slot) {
                continue;
            }
            let parent = self.data(slot).parent.get();
            if parent.is_invalid() {
                continue;
            }
            if !self.node_is_partial_relayout_boundary(slot) || !collect_boundary(slot, false) {
                return None;
            }
        }

        for &rebuilt_root in rebuilt_subtree_root_slots {
            // Every rebuilt subtree must lie inside a boundary for its dirt to be confined.
            // The rebuilt box itself may qualify with its committed row still pending; boundaries
            // above it were not replaced and must have one.
            let containing_boundary = self.nearest_inclusive_partial_relayout_boundary(rebuilt_root)?;
            if !collect_boundary(containing_boundary, containing_boundary == rebuilt_root) {
                return None;
            }
        }

        // A root nested inside another root is relaid out as part of the ancestor's subtree.
        partial_relayout_roots.retain(|&root| {
            let mut ancestor = self.data(root).parent.get();
            while !ancestor.is_invalid() {
                if collected_boundaries.contains(&ancestor) {
                    return false;
                }
                ancestor = self.data(ancestor).parent.get();
            }
            true
        });

        if partial_relayout_roots.is_empty() {
            return None;
        }
        Some(partial_relayout_roots)
    }

    pub(crate) fn node_can_replay_saved_abspos_layout_inputs_after_style_change(&self, node: NodeSlotId) -> bool {
        let data = self.data(node);

        if data.containing_block.get().is_invalid() || !self.slot_is_live(data.containing_block.get()) {
            return false;
        }

        if node_facts::has_flag(data, NodeFlag::SavedAbsposCbDerivesFromOwnComputedValues) {
            return false;
        }

        let Some(style) = node_facts::node_style_view(data) else {
            return false;
        };
        let inset = &style.surround().inset;
        let uses_static_position =
            (inset.left.is_auto() && inset.right.is_auto()) || (inset.top.is_auto() && inset.bottom.is_auto());
        if uses_static_position
            && node_facts::has_flag(data, NodeFlag::SavedAbsposAlignmentDerivesFromOwnComputedValues)
        {
            return false;
        }

        true
    }

    pub(crate) fn reset_cached_intrinsic_sizes_of_self_and_ancestors(&self, node: NodeSlotId) {
        let node_kind = self.data(node).kind.get();
        if node_facts::kind_is_box(node_kind) {
            self.reset_cached_intrinsic_sizes(node);
        }
        self.reset_cached_intrinsic_sizes_of_ancestors(node);
    }

    // Reset intrinsic size caches for ancestors up to abspos or SVG root boundary.
    // Absolutely positioned elements don't contribute to ancestor intrinsic sizes,
    // so changes inside an abspos box don't require resetting ancestor caches.
    // SVG root elements have intrinsic sizes determined solely by their own attributes
    // (width, height, viewBox), not by their children, so the same logic applies.
    fn reset_cached_intrinsic_sizes_of_ancestors(&self, node: NodeSlotId) {
        let mut ancestor = self.data(node).parent.get();
        while !ancestor.is_invalid() {
            let ancestor_data = self.data(ancestor);
            let (ancestor_kind, ancestor_parent) = { (ancestor_data.kind.get(), ancestor_data.parent.get()) };
            if node_facts::kind_is_box(ancestor_kind) {
                self.reset_cached_intrinsic_sizes(ancestor);
                let ancestor_is_absolutely_positioned = self
                    .node_style_if_live(ancestor)
                    .is_some_and(|style| style.is_absolutely_positioned());
                if ancestor_is_absolutely_positioned || ancestor_kind == NodeKind::SVGSVGBox {
                    break;
                }
            }
            ancestor = ancestor_parent;
        }
    }

    pub(crate) fn set_needs_layout_update(&self, node: NodeSlotId, propagate_through_ancestors: bool) {
        // Bumped before the already-dirty early return below: a dirty node does not imply its
        // whole ancestor chain was bumped for the current epoch values, and over-bumping is free.
        self.bump_fragment_cache_epoch_of_self_and_ancestors(node);

        let data = self.data(node);
        let (node_was_already_dirty, node_is_box, first_child, parent) = {
            (
                data.flags.get() & NodeFlag::NeedsLayoutUpdate as u32 != 0,
                node_facts::kind_is_box(data.kind.get()),
                data.first_child.get(),
                data.parent.get(),
            )
        };

        if node_was_already_dirty && propagate_through_ancestors {
            // A dirty node normally implies dirty ancestors, but the walk that marked a partial
            // relayout boundary stopped there and left its ancestors clean, so a through-ancestors
            // invalidation arriving on the boundary itself must still walk and mark them.
            if !node_is_box || !self.node_is_partial_relayout_boundary(node) {
                return;
            }
        }

        if !node_was_already_dirty {
            self.set_node_flag(node, NodeFlag::NeedsLayoutUpdate, true);
            // Relayout may rebuild an identical fragment whose cached paint output the commit diff
            // then keeps, even when what this node paints changed (its image data arrived).
            self.invalidate_paint_cache(node);
        }

        if node_is_box {
            self.reset_cached_intrinsic_sizes(node);
        }

        // Mark any anonymous children generated by this node for layout update.
        // NOTE: if this node generated an anonymous parent, all ancestors are indiscriminately marked below.
        let fragment_cache_epochs_enabled =
            super::fc_run_cache::fc_run_cache_mode_from_environment() != super::fc_run_cache::FcRunCacheMode::Disabled;
        let mut child = first_child;
        while !child.is_invalid() {
            let child_data = self.data(child);
            let (child_kind, child_is_anonymous, next_sibling) = {
                (
                    child_data.kind.get(),
                    child_data.flags.get() & NodeFlag::Anonymous as u32 != 0,
                    child_data.next_sibling.get(),
                )
            };
            if node_facts::kind_is_box(child_kind) && child_is_anonymous && child_kind != NodeKind::TableWrapper {
                if fragment_cache_epochs_enabled {
                    child_data
                        .fragment_cache_epoch
                        .set(child_data.fragment_cache_epoch.get().wrapping_add(1));
                }
                self.set_node_flag(child, NodeFlag::NeedsLayoutUpdate, true);
                self.reset_cached_intrinsic_sizes(child);
            }
            child = next_sibling;
        }

        if !propagate_through_ancestors {
            self.register_partial_relayout_boundary_root(node);
            return;
        }

        let mut ancestor = parent;
        while !ancestor.is_invalid() {
            let ancestor_data = self.data(ancestor);
            let (ancestor_kind, ancestor_is_dirty, ancestor_parent) = {
                (
                    ancestor_data.kind.get(),
                    ancestor_data.flags.get() & NodeFlag::NeedsLayoutUpdate as u32 != 0,
                    ancestor_data.parent.get(),
                )
            };
            if ancestor_is_dirty {
                break;
            }
            self.set_node_flag(ancestor, NodeFlag::NeedsLayoutUpdate, true);
            if node_facts::kind_is_box(ancestor_kind) && self.node_is_partial_relayout_boundary(ancestor) {
                self.register_partial_relayout_boundary_root(ancestor);
                break;
            }
            ancestor = ancestor_parent;
        }

        self.reset_cached_intrinsic_sizes_of_ancestors(node);
    }
}

/// # Safety
///
/// The arena must remain valid for the duration of the call, and `node` must name a live node
/// in this arena.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_node_is_partial_relayout_boundary(arena: *mut c_void, node: NodeSlotId) -> bool {
    // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
    unsafe { LayoutNodeArena::from_handle(arena) }.node_is_partial_relayout_boundary(node)
}

/// # Safety
///
/// The arena must remain valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_has_partial_relayout_boundary_roots(arena: *mut c_void) -> bool {
    // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
    unsafe { LayoutNodeArena::from_handle(arena) }.has_partial_relayout_boundary_roots()
}

/// # Safety
///
/// The arena must remain valid for the duration of the call. The drained slot ids may name
/// freed slots; the caller resolves liveness before dereferencing anything.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_take_partial_relayout_boundary_roots(
    arena: *mut c_void,
    context: *mut c_void,
    push_root: unsafe extern "C" fn(*mut c_void, NodeSlotId),
) {
    // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
    let arena = unsafe { LayoutNodeArena::from_handle(arena) };
    for root in arena.take_partial_relayout_boundary_roots() {
        // SAFETY: The C++ callback appends the slot id to a caller-owned collection.
        unsafe { push_root(context, root) };
    }
}

/// The facts the host owns that take an update off the partial relayout path.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiPartialRelayoutHostFacts {
    pub document_needs_full_layout_tree_update: bool,
    pub container_query_evaluation_is_pending: bool,
    pub should_collect_devtools_layout_data: bool,
    pub any_anchor_names_are_registered: bool,
}

/// # Safety
///
/// `slots` must be valid for `count` elements whenever `count` is nonzero.
unsafe fn slot_slice<'a>(slots: *const NodeSlotId, count: usize) -> &'a [NodeSlotId] {
    if count == 0 {
        &[]
    } else {
        // SAFETY: A nonzero count implies a valid array of that length.
        unsafe { std::slice::from_raw_parts(slots, count) }
    }
}

/// # Safety
///
/// The arena must remain valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_record_partial_relayout_escape(arena: *mut c_void) {
    // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
    unsafe { LayoutNodeArena::from_handle(arena) }.record_partial_relayout_escape();
}

/// # Safety
///
/// The arena must remain valid for the duration of the call, and the slot array must be valid
/// for its count; its slots may be stale. `root` may be the invalid slot id.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_partial_relayout_may_be_attempted(
    arena: *mut c_void,
    root: NodeSlotId,
    registered_root_slots: *const NodeSlotId,
    registered_root_count: usize,
    facts: FfiPartialRelayoutHostFacts,
) -> bool {
    // SAFETY: The C++ caller keeps the arena and the slot array alive for this call.
    let arena = unsafe { LayoutNodeArena::from_handle(arena) };
    let registered = unsafe { slot_slice(registered_root_slots, registered_root_count) };
    arena.partial_relayout_may_be_attempted(root, registered, facts)
}

/// # Safety
///
/// The arena must remain valid for the duration of the call, and both slot arrays must be
/// valid for their counts. Slots in `registered_root_slots` may be stale; slots in
/// `rebuilt_subtree_root_slots` and `root` must name live nodes in this arena.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_plan_partial_relayout(
    arena: *mut c_void,
    root: NodeSlotId,
    registered_root_slots: *const NodeSlotId,
    registered_root_count: usize,
    rebuilt_subtree_root_slots: *const NodeSlotId,
    rebuilt_subtree_root_count: usize,
    layout_tree_update_escaped_rebuild_roots: bool,
    context: *mut c_void,
    push_root: unsafe extern "C" fn(*mut c_void, NodeSlotId),
) -> bool {
    // SAFETY: The C++ caller keeps the arena and both slot arrays alive for this call.
    let arena = unsafe { LayoutNodeArena::from_handle(arena) };
    let registered = unsafe { slot_slice(registered_root_slots, registered_root_count) };
    let rebuilt = unsafe { slot_slice(rebuilt_subtree_root_slots, rebuilt_subtree_root_count) };
    match arena.plan_partial_relayout(root, registered, rebuilt, layout_tree_update_escaped_rebuild_roots) {
        Some(roots) => {
            for root in roots {
                // SAFETY: The C++ callback appends the slot id to a caller-owned collection.
                unsafe { push_root(context, root) };
            }
            true
        }
        None => false,
    }
}

/// # Safety
///
/// The arena must remain valid for the duration of the call, and `node` must name a live node
/// in this arena.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_reset_cached_intrinsic_sizes_of_self_and_ancestors(
    arena: *mut c_void,
    node: NodeSlotId,
) {
    // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
    unsafe { LayoutNodeArena::from_handle(arena) }.reset_cached_intrinsic_sizes_of_self_and_ancestors(node);
}

/// # Safety
///
/// The arena must remain valid for the duration of the call, and `node` must name a live node
/// in this arena.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_classify_layout_tree_update(
    arena: *mut c_void,
    node: NodeSlotId,
    reason_is_structural_boundary_self_rebuild: bool,
) -> FfiLayoutTreeUpdateClassification {
    // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
    unsafe { LayoutNodeArena::from_handle(arena) }
        .classify_layout_tree_update(node, reason_is_structural_boundary_self_rebuild)
}

/// # Safety
///
/// The arena must remain valid for the duration of the call, and `node` must name a live node
/// in this arena.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_needs_layout_update(
    arena: *mut c_void,
    node: NodeSlotId,
    propagate_through_ancestors: bool,
) {
    // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
    unsafe { LayoutNodeArena::from_handle(arena) }.set_needs_layout_update(node, propagate_through_ancestors);
}

#[cfg(test)]
mod tests {
    use super::FfiPartialRelayoutHostFacts;
    use crate::layout::layout_node_arena::{LayoutNodeArena, NodeAllocation};
    use crate::layout::node_data::{NodeFlag, NodeKind, NodeSlotId};

    fn allocate_box_with_a_dummy_shell(arena: &mut LayoutNodeArena) -> NodeAllocation {
        let allocation = arena.allocate_for_test();
        arena.data(allocation.slot).kind.set(NodeKind::Box);
        arena.data(allocation.slot).shell.set(std::ptr::dangling_mut());
        allocation
    }

    fn free_node(arena: &mut LayoutNodeArena, allocation: &NodeAllocation) {
        arena
            .free_subtree(allocation.slot)
            .destroy_shells_and_invoke_callbacks();
    }

    #[test]
    fn a_box_without_a_committed_row_or_splice_derivable_ancestors_is_not_a_boundary() {
        let mut arena = LayoutNodeArena::new();
        let allocation = arena.allocate_for_test();
        arena.data(allocation.slot).kind.set(NodeKind::Box);
        assert!(!arena.node_is_partial_relayout_boundary(allocation.slot));
        free_node(&mut arena, &allocation);
    }

    #[test]
    fn registering_a_boundary_root_twice_yields_a_single_drained_entry() {
        let mut arena = LayoutNodeArena::new();
        let allocation = allocate_box_with_a_dummy_shell(&mut arena);
        assert!(!arena.has_partial_relayout_boundary_roots());
        arena.register_partial_relayout_boundary_root(allocation.slot);
        arena.register_partial_relayout_boundary_root(allocation.slot);
        assert!(arena.has_partial_relayout_boundary_roots());
        assert_eq!(arena.take_partial_relayout_boundary_roots(), vec![allocation.slot]);
        assert!(!arena.has_partial_relayout_boundary_roots());
        free_node(&mut arena, &allocation);
    }

    #[test]
    fn a_freed_boundary_root_still_counts_as_pending_but_resolves_to_a_dead_shell() {
        let mut arena = LayoutNodeArena::new();
        let allocation = allocate_box_with_a_dummy_shell(&mut arena);
        arena.register_partial_relayout_boundary_root(allocation.slot);
        free_node(&mut arena, &allocation);
        assert!(arena.has_partial_relayout_boundary_roots());
        let reused = allocate_box_with_a_dummy_shell(&mut arena);
        let drained = arena.take_partial_relayout_boundary_roots();
        assert_eq!(drained, vec![allocation.slot]);
        assert!(!arena.slot_is_live(allocation.slot));
        assert!(arena.slot_is_live(reused.slot));
        free_node(&mut arena, &reused);
    }

    #[test]
    fn registering_a_boundary_root_prunes_entries_for_freed_nodes() {
        let mut arena = LayoutNodeArena::new();
        let first = allocate_box_with_a_dummy_shell(&mut arena);
        arena.register_partial_relayout_boundary_root(first.slot);
        free_node(&mut arena, &first);
        let second = allocate_box_with_a_dummy_shell(&mut arena);
        arena.register_partial_relayout_boundary_root(second.slot);
        assert_eq!(arena.take_partial_relayout_boundary_roots(), vec![second.slot]);
        free_node(&mut arena, &second);
    }

    fn node_is_dirty(arena: &LayoutNodeArena, allocation: &NodeAllocation) -> bool {
        arena.data(allocation.slot).flags.get() & NodeFlag::NeedsLayoutUpdate as u32 != 0
    }

    fn intrinsic_cache_epoch(arena: &LayoutNodeArena, allocation: &NodeAllocation) -> u16 {
        arena.data(allocation.slot).intrinsic_cache_epoch.get()
    }

    #[test]
    fn marking_a_node_marks_clean_ancestors_and_bumps_their_intrinsic_epochs() {
        let mut arena = LayoutNodeArena::new();
        let parent = allocate_box_with_a_dummy_shell(&mut arena);
        let child = allocate_box_with_a_dummy_shell(&mut arena);
        arena.insert_child(parent.slot, child.slot, NodeSlotId::INVALID);

        arena.set_needs_layout_update(child.slot, true);

        assert!(node_is_dirty(&arena, &child));
        assert!(node_is_dirty(&arena, &parent));
        assert_eq!(intrinsic_cache_epoch(&arena, &child), 1);
        assert_eq!(intrinsic_cache_epoch(&arena, &parent), 1);
        free_node(&mut arena, &parent);
    }

    #[test]
    fn an_already_dirty_ancestor_stops_the_marking_walk() {
        let mut arena = LayoutNodeArena::new();
        let grandparent = allocate_box_with_a_dummy_shell(&mut arena);
        let parent = allocate_box_with_a_dummy_shell(&mut arena);
        let child = allocate_box_with_a_dummy_shell(&mut arena);
        arena.insert_child(grandparent.slot, parent.slot, NodeSlotId::INVALID);
        arena.insert_child(parent.slot, child.slot, NodeSlotId::INVALID);
        arena.set_node_flag(parent.slot, NodeFlag::NeedsLayoutUpdate, true);

        arena.set_needs_layout_update(child.slot, true);

        assert!(node_is_dirty(&arena, &child));
        assert!(!node_is_dirty(&arena, &grandparent));
        free_node(&mut arena, &grandparent);
    }

    #[test]
    fn remarking_a_dirty_non_boundary_node_leaves_its_parent_clean() {
        let mut arena = LayoutNodeArena::new();
        let parent = allocate_box_with_a_dummy_shell(&mut arena);
        let child = allocate_box_with_a_dummy_shell(&mut arena);
        arena.insert_child(parent.slot, child.slot, NodeSlotId::INVALID);
        arena.set_node_flag(child.slot, NodeFlag::NeedsLayoutUpdate, true);

        arena.set_needs_layout_update(child.slot, true);

        assert!(!node_is_dirty(&arena, &parent));
        assert_eq!(intrinsic_cache_epoch(&arena, &parent), 0);
        free_node(&mut arena, &parent);
    }

    #[test]
    fn only_anonymous_non_table_wrapper_box_children_are_marked_alongside_their_parent() {
        let mut arena = LayoutNodeArena::new();
        let parent = allocate_box_with_a_dummy_shell(&mut arena);
        let anonymous_child = allocate_box_with_a_dummy_shell(&mut arena);
        let anonymous_table_wrapper_child = allocate_box_with_a_dummy_shell(&mut arena);
        let named_child = allocate_box_with_a_dummy_shell(&mut arena);
        arena
            .data(anonymous_child.slot)
            .flags
            .set(arena.data(anonymous_child.slot).flags.get() | (NodeFlag::Anonymous as u32));
        arena
            .data(anonymous_table_wrapper_child.slot)
            .flags
            .set(arena.data(anonymous_table_wrapper_child.slot).flags.get() | (NodeFlag::Anonymous as u32));
        arena
            .data(anonymous_table_wrapper_child.slot)
            .kind
            .set(NodeKind::TableWrapper);
        arena.insert_child(parent.slot, anonymous_child.slot, NodeSlotId::INVALID);
        arena.insert_child(parent.slot, anonymous_table_wrapper_child.slot, NodeSlotId::INVALID);
        arena.insert_child(parent.slot, named_child.slot, NodeSlotId::INVALID);

        arena.set_needs_layout_update(parent.slot, true);

        assert!(node_is_dirty(&arena, &parent));
        assert!(node_is_dirty(&arena, &anonymous_child));
        assert!(!node_is_dirty(&arena, &anonymous_table_wrapper_child));
        assert!(!node_is_dirty(&arena, &named_child));
        free_node(&mut arena, &parent);
    }

    #[test]
    fn collecting_roots_skips_stale_registered_slots_and_reports_no_eligible_roots() {
        let mut arena = LayoutNodeArena::new();
        let freed = allocate_box_with_a_dummy_shell(&mut arena);
        let stale_slot = freed.slot;
        free_node(&mut arena, &freed);
        assert_eq!(arena.collect_partial_relayout_roots(&[stale_slot], &[]), None);
    }

    #[test]
    fn collecting_roots_fails_for_a_rebuilt_subtree_with_no_containing_boundary() {
        let mut arena = LayoutNodeArena::new();
        let parent = allocate_box_with_a_dummy_shell(&mut arena);
        let child = allocate_box_with_a_dummy_shell(&mut arena);
        arena.insert_child(parent.slot, child.slot, NodeSlotId::INVALID);
        assert_eq!(arena.collect_partial_relayout_roots(&[], &[child.slot]), None);
        free_node(&mut arena, &parent);
    }

    #[test]
    fn layout_tree_update_classification_escalates_past_anonymous_parents_only() {
        let mut arena = LayoutNodeArena::new();
        let grandparent = allocate_box_with_a_dummy_shell(&mut arena);
        let anonymous_parent = allocate_box_with_a_dummy_shell(&mut arena);
        let child = allocate_box_with_a_dummy_shell(&mut arena);
        arena
            .data(anonymous_parent.slot)
            .flags
            .set(arena.data(anonymous_parent.slot).flags.get() | (NodeFlag::Anonymous as u32));
        arena.insert_child(grandparent.slot, anonymous_parent.slot, NodeSlotId::INVALID);
        arena.insert_child(anonymous_parent.slot, child.slot, NodeSlotId::INVALID);

        let classification = arena.classify_layout_tree_update(child.slot, false);
        assert!(!arena.pending_updates_escape_partial_relayout.get());
        assert!(!classification.marks_partial_relayout_boundary_self_only);
        assert_eq!(
            classification.nearest_non_anonymous_ancestor_when_parent_is_anonymous,
            grandparent.slot
        );

        let parent_classification = arena.classify_layout_tree_update(anonymous_parent.slot, false);
        assert_eq!(
            parent_classification.nearest_non_anonymous_ancestor_when_parent_is_anonymous,
            NodeSlotId::INVALID
        );

        arena.classify_layout_tree_update(grandparent.slot, false);
        assert!(arena.pending_updates_escape_partial_relayout.get());
        free_node(&mut arena, &grandparent);
    }

    fn host_facts_permitting_partial_relayout() -> FfiPartialRelayoutHostFacts {
        FfiPartialRelayoutHostFacts {
            document_needs_full_layout_tree_update: false,
            container_query_evaluation_is_pending: false,
            should_collect_devtools_layout_data: false,
            any_anchor_names_are_registered: false,
        }
    }

    // A parent box with one child registered as a boundary root, drained like a pass would.
    fn arena_with_a_registered_child_root() -> (LayoutNodeArena, NodeAllocation, Vec<NodeSlotId>) {
        let mut arena = LayoutNodeArena::new();
        let parent = allocate_box_with_a_dummy_shell(&mut arena);
        let child = allocate_box_with_a_dummy_shell(&mut arena);
        arena.insert_child(parent.slot, child.slot, NodeSlotId::INVALID);
        arena.set_needs_layout_update(child.slot, false);
        let registered = arena.take_partial_relayout_boundary_roots();
        assert_eq!(registered, vec![child.slot]);
        (arena, parent, registered)
    }

    #[test]
    fn a_recorded_escape_refuses_the_precheck_until_a_plan_consumes_it() {
        let (mut arena, parent, registered) = arena_with_a_registered_child_root();
        let facts = host_facts_permitting_partial_relayout();

        assert!(arena.partial_relayout_may_be_attempted(parent.slot, &registered, facts));
        arena.record_partial_relayout_escape();
        assert!(!arena.partial_relayout_may_be_attempted(parent.slot, &registered, facts));

        assert_eq!(arena.plan_partial_relayout(parent.slot, &registered, &[], false), None);
        assert!(!arena.pending_updates_escape_partial_relayout.get());
        assert!(arena.partial_relayout_may_be_attempted(parent.slot, &registered, facts));
        free_node(&mut arena, &parent);
    }

    #[test]
    fn planning_refuses_when_the_build_escaped_its_rebuild_roots() {
        let (mut arena, parent, registered) = arena_with_a_registered_child_root();
        assert_eq!(arena.plan_partial_relayout(parent.slot, &registered, &[], true), None);
        assert!(!arena.pending_updates_escape_partial_relayout.get());
        free_node(&mut arena, &parent);
    }

    #[test]
    fn a_dirty_root_refuses_both_the_precheck_and_the_plan() {
        let (mut arena, parent, registered) = arena_with_a_registered_child_root();
        arena.set_node_flag(parent.slot, NodeFlag::NeedsLayoutUpdate, true);
        assert!(!arena.partial_relayout_may_be_attempted(
            parent.slot,
            &registered,
            host_facts_permitting_partial_relayout()
        ));
        assert_eq!(arena.plan_partial_relayout(parent.slot, &registered, &[], false), None);
        free_node(&mut arena, &parent);
    }

    #[test]
    fn the_precheck_refuses_a_missing_root_an_empty_root_set_and_every_host_fact() {
        let (mut arena, parent, registered) = arena_with_a_registered_child_root();
        let permitted = host_facts_permitting_partial_relayout();

        assert!(!arena.partial_relayout_may_be_attempted(NodeSlotId::INVALID, &registered, permitted));
        assert!(!arena.partial_relayout_may_be_attempted(parent.slot, &[], permitted));

        let refusing_facts = [
            FfiPartialRelayoutHostFacts {
                document_needs_full_layout_tree_update: true,
                ..permitted
            },
            FfiPartialRelayoutHostFacts {
                container_query_evaluation_is_pending: true,
                ..permitted
            },
            FfiPartialRelayoutHostFacts {
                should_collect_devtools_layout_data: true,
                ..permitted
            },
            FfiPartialRelayoutHostFacts {
                any_anchor_names_are_registered: true,
                ..permitted
            },
        ];
        for facts in refusing_facts {
            assert!(!arena.partial_relayout_may_be_attempted(parent.slot, &registered, facts));
        }
        free_node(&mut arena, &parent);
    }

    #[test]
    fn boundary_self_only_marking_records_the_root_and_leaves_the_parent_clean() {
        let mut arena = LayoutNodeArena::new();
        let parent = allocate_box_with_a_dummy_shell(&mut arena);
        let child = allocate_box_with_a_dummy_shell(&mut arena);
        arena.insert_child(parent.slot, child.slot, NodeSlotId::INVALID);

        arena.set_needs_layout_update(child.slot, false);

        assert!(node_is_dirty(&arena, &child));
        assert!(!node_is_dirty(&arena, &parent));
        assert_eq!(arena.take_partial_relayout_boundary_roots(), vec![child.slot]);
        free_node(&mut arena, &parent);
    }
}
