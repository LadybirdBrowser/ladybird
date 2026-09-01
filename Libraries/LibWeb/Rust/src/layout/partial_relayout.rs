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

#[cfg(test)]
mod tests {
    use crate::layout::layout_node_arena::{LayoutNodeArena, NodeAllocation};
    use crate::layout::node_data::NodeKind;

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
}
