/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::abort_on_panic;
use crate::layout::LayoutNodeArena;
use crate::layout::formatting_context::{FfiFormattingContextType, formatting_context_type_created_by_node_data};
use crate::layout::node_data::{NodeFlag, NodeKind, NodeSlotId};
use crate::layout::node_facts;
use std::ffi::c_void;

impl LayoutNodeArena {
    fn commit_splice_position_is_derivable_from_layout_ancestors(&self, node: NodeSlotId) -> bool {
        let paintable_rows = self.paintable_rows();
        // SAFETY: data() generation-checks every slot the walk visits.
        let mut ancestor = unsafe { (*self.data(node)).parent };
        while !ancestor.is_invalid() {
            if paintable_rows.paintable_row_is_populated(ancestor) {
                return true;
            }
            // SAFETY: data() generation-checks every slot the walk visits.
            let ancestor_data = unsafe { &*self.data(ancestor) };
            if !node_facts::node_is_fragmented_inline(ancestor_data, node_facts::node_style_view(ancestor_data)) {
                return false;
            }
            ancestor = ancestor_data.parent;
        }
        false
    }

    pub(crate) fn node_is_partial_relayout_boundary(&self, node: NodeSlotId) -> bool {
        // SAFETY: The caller supplies a live slot; data() generation-checks it.
        let data = unsafe { &*self.data(node) };

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
        if data.kind == NodeKind::SVGSVGBox && !style_is_absolutely_positioned {
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

        let parent_style = (!data.parent.is_invalid())
            .then(|| self.style_payloads(data.parent))
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
        // SAFETY: The caller supplies a live slot; data() generation-checks it.
        let kind = unsafe { (*self.data(node)).kind };
        assert!(node_facts::kind_is_box(kind));
        let mut roots = self.partial_relayout_boundary_roots.borrow_mut();
        roots.retain(|candidate| !self.shell_if_live(*candidate).is_null());
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

    pub(crate) fn node_can_replay_saved_abspos_layout_inputs_after_style_change(&self, node: NodeSlotId) -> bool {
        // SAFETY: The caller supplies a live slot; data() generation-checks it.
        let data = unsafe { &*self.data(node) };

        if data.containing_block.is_invalid() || !self.slot_is_live(data.containing_block) {
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
        // SAFETY: data() generation-checks the slot; the raw read ends before the epoch write.
        let node_kind = unsafe { (&raw const (*self.data(node)).kind).read() };
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
        // SAFETY: data() generation-checks every slot the walk visits; the raw reads
        // end before the epoch write for the same node.
        let mut ancestor = unsafe { (&raw const (*self.data(node)).parent).read() };
        while !ancestor.is_invalid() {
            let ancestor_data = self.data(ancestor);
            // SAFETY: As above.
            let (ancestor_kind, ancestor_parent) = unsafe {
                (
                    (&raw const (*ancestor_data).kind).read(),
                    (&raw const (*ancestor_data).parent).read(),
                )
            };
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
        // SAFETY: data() generation-checks the slot; the raw reads end before any write.
        let (node_was_already_dirty, node_is_box, first_child, parent) = unsafe {
            (
                (&raw const (*data).flags).read() & NodeFlag::NeedsLayoutUpdate as u32 != 0,
                node_facts::kind_is_box((&raw const (*data).kind).read()),
                (&raw const (*data).first_child).read(),
                (&raw const (*data).parent).read(),
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
            // SAFETY: data() generation-checks every slot the walk visits; the raw reads
            // end before the flag and epoch writes for the same child.
            let (child_kind, child_is_anonymous, next_sibling) = unsafe {
                (
                    (&raw const (*child_data).kind).read(),
                    (&raw const (*child_data).flags).read() & NodeFlag::Anonymous as u32 != 0,
                    (&raw const (*child_data).next_sibling).read(),
                )
            };
            if node_facts::kind_is_box(child_kind) && child_is_anonymous && child_kind != NodeKind::TableWrapper {
                if fragment_cache_epochs_enabled {
                    // SAFETY: As above; layout serializes mutation on the arena's owner thread.
                    unsafe {
                        let epoch = &raw mut (*child_data).fragment_cache_epoch;
                        epoch.write(epoch.read().wrapping_add(1));
                    }
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
            // SAFETY: data() generation-checks every slot the walk visits; the raw reads
            // end before the flag write for the same ancestor.
            let (ancestor_kind, ancestor_is_dirty, ancestor_parent) = unsafe {
                (
                    (&raw const (*ancestor_data).kind).read(),
                    (&raw const (*ancestor_data).flags).read() & NodeFlag::NeedsLayoutUpdate as u32 != 0,
                    (&raw const (*ancestor_data).parent).read(),
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
    abort_on_panic(|| {
        // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
        unsafe { LayoutNodeArena::from_handle(arena) }.node_is_partial_relayout_boundary(node)
    })
}

/// # Safety
///
/// The arena must remain valid for the duration of the call, and `node` must name a live node
/// in this arena.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_register_partial_relayout_boundary_root(arena: *mut c_void, node: NodeSlotId) {
    abort_on_panic(|| {
        // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
        unsafe { LayoutNodeArena::from_handle(arena) }.register_partial_relayout_boundary_root(node);
    });
}

/// # Safety
///
/// The arena must remain valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_has_partial_relayout_boundary_roots(arena: *mut c_void) -> bool {
    abort_on_panic(|| {
        // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
        unsafe { LayoutNodeArena::from_handle(arena) }.has_partial_relayout_boundary_roots()
    })
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
    abort_on_panic(|| {
        // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
        let arena = unsafe { LayoutNodeArena::from_handle(arena) };
        for root in arena.take_partial_relayout_boundary_roots() {
            // SAFETY: The C++ callback appends the slot id to a caller-owned collection.
            unsafe { push_root(context, root) };
        }
    });
}

/// # Safety
///
/// The arena must remain valid for the duration of the call, and `node` must name a live node
/// in this arena.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_node_can_replay_saved_abspos_layout_inputs_after_style_change(
    arena: *mut c_void,
    node: NodeSlotId,
) -> bool {
    abort_on_panic(|| {
        // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
        unsafe { LayoutNodeArena::from_handle(arena) }
            .node_can_replay_saved_abspos_layout_inputs_after_style_change(node)
    })
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
    abort_on_panic(|| {
        // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
        unsafe { LayoutNodeArena::from_handle(arena) }.reset_cached_intrinsic_sizes_of_self_and_ancestors(node);
    });
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
    abort_on_panic(|| {
        // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
        unsafe { LayoutNodeArena::from_handle(arena) }.set_needs_layout_update(node, propagate_through_ancestors);
    });
}

#[cfg(test)]
mod tests {
    use crate::layout::layout_node_arena::{LayoutNodeArena, NodeAllocation};
    use crate::layout::node_data::{NodeFlag, NodeKind, NodeSlotId};

    fn allocate_box_with_a_dummy_shell(arena: &mut LayoutNodeArena) -> NodeAllocation {
        let allocation = arena.allocate();
        // SAFETY: allocate() returned this live slot's data pointer. The dummy shell
        // pointer is only ever compared against null, never dereferenced.
        unsafe {
            (*allocation.data).kind = NodeKind::Box;
            (*allocation.data).shell = allocation.data.cast();
        }
        allocation
    }

    fn free_node(arena: &mut LayoutNodeArena, allocation: &NodeAllocation) {
        arena
            .free(allocation.slot, allocation.generation)
            .detached_children
            .release_all();
    }

    #[test]
    fn a_box_without_a_committed_row_or_splice_derivable_ancestors_is_not_a_boundary() {
        let mut arena = LayoutNodeArena::new();
        let allocation = arena.allocate();
        // SAFETY: allocate() returned this live slot's data pointer.
        unsafe {
            (*allocation.data).kind = NodeKind::Box;
        }
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
        assert!(arena.shell_if_live(allocation.slot).is_null());
        assert!(!arena.shell_if_live(reused.slot).is_null());
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

    fn node_is_dirty(allocation: &NodeAllocation) -> bool {
        // SAFETY: The allocation is still live, so its data pointer addresses the arena slot.
        unsafe { (*allocation.data).flags & NodeFlag::NeedsLayoutUpdate as u32 != 0 }
    }

    fn intrinsic_cache_epoch(allocation: &NodeAllocation) -> u16 {
        // SAFETY: The allocation is still live, so its data pointer addresses the arena slot.
        unsafe { (*allocation.data).intrinsic_cache_epoch }
    }

    #[test]
    fn marking_a_node_marks_clean_ancestors_and_bumps_their_intrinsic_epochs() {
        let mut arena = LayoutNodeArena::new();
        let parent = allocate_box_with_a_dummy_shell(&mut arena);
        let child = allocate_box_with_a_dummy_shell(&mut arena);
        arena.insert_child(parent.slot, child.slot, NodeSlotId::INVALID);

        arena.set_needs_layout_update(child.slot, true);

        assert!(node_is_dirty(&child));
        assert!(node_is_dirty(&parent));
        assert_eq!(intrinsic_cache_epoch(&child), 1);
        assert_eq!(intrinsic_cache_epoch(&parent), 1);
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

        assert!(node_is_dirty(&child));
        assert!(!node_is_dirty(&grandparent));
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

        assert!(!node_is_dirty(&parent));
        assert_eq!(intrinsic_cache_epoch(&parent), 0);
        free_node(&mut arena, &parent);
    }

    #[test]
    fn only_anonymous_non_table_wrapper_box_children_are_marked_alongside_their_parent() {
        let mut arena = LayoutNodeArena::new();
        let parent = allocate_box_with_a_dummy_shell(&mut arena);
        let anonymous_child = allocate_box_with_a_dummy_shell(&mut arena);
        let anonymous_table_wrapper_child = allocate_box_with_a_dummy_shell(&mut arena);
        let named_child = allocate_box_with_a_dummy_shell(&mut arena);
        // SAFETY: allocate() returned these live slots' data pointers.
        unsafe {
            (*anonymous_child.data).flags |= NodeFlag::Anonymous as u32;
            (*anonymous_table_wrapper_child.data).flags |= NodeFlag::Anonymous as u32;
            (*anonymous_table_wrapper_child.data).kind = NodeKind::TableWrapper;
        }
        arena.insert_child(parent.slot, anonymous_child.slot, NodeSlotId::INVALID);
        arena.insert_child(parent.slot, anonymous_table_wrapper_child.slot, NodeSlotId::INVALID);
        arena.insert_child(parent.slot, named_child.slot, NodeSlotId::INVALID);

        arena.set_needs_layout_update(parent.slot, true);

        assert!(node_is_dirty(&parent));
        assert!(node_is_dirty(&anonymous_child));
        assert!(!node_is_dirty(&anonymous_table_wrapper_child));
        assert!(!node_is_dirty(&named_child));
        free_node(&mut arena, &parent);
    }

    #[test]
    fn boundary_self_only_marking_records_the_root_and_leaves_the_parent_clean() {
        let mut arena = LayoutNodeArena::new();
        let parent = allocate_box_with_a_dummy_shell(&mut arena);
        let child = allocate_box_with_a_dummy_shell(&mut arena);
        arena.insert_child(parent.slot, child.slot, NodeSlotId::INVALID);

        arena.set_needs_layout_update(child.slot, false);

        assert!(node_is_dirty(&child));
        assert!(!node_is_dirty(&parent));
        assert_eq!(arena.take_partial_relayout_boundary_roots(), vec![child.slot]);
        free_node(&mut arena, &parent);
    }
}
