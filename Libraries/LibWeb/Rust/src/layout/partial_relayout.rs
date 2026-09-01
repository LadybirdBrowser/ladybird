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

#[cfg(test)]
mod tests {
    use crate::layout::layout_node_arena::LayoutNodeArena;
    use crate::layout::node_data::NodeKind;

    #[test]
    fn a_box_without_a_committed_row_or_splice_derivable_ancestors_is_not_a_boundary() {
        let mut arena = LayoutNodeArena::new();
        let allocation = arena.allocate();
        // SAFETY: allocate() returned this live slot's data pointer.
        unsafe {
            (*allocation.data).kind = NodeKind::Box;
        }
        assert!(!arena.node_is_partial_relayout_boundary(allocation.slot));
        arena
            .free(allocation.slot, allocation.generation)
            .detached_children
            .release_all();
    }
}
