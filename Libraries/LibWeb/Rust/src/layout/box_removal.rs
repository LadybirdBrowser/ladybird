/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A DOM node's box leaving the box of its parent as the node is removed. The render owner settles it as it applies the
//! removal, where it reads the boxes as of every write the host made before: the box is detached in place where the
//! parent's box keeps its structure without it, and the parent is marked for the layout tree build to rebuild
//! otherwise. The host streams the removal and waits for neither.

use super::LayoutNodeArena;
use super::layout_changes::LayoutWrite;
use super::node_data::{NodeFlag, NodeSlotId};
use super::node_facts::{self, has_flag};
use super::tree_builder::node_kind_is_node_with_style;
use super::tree_mutation::HostWorkDue;
use super::tree_update_marks::FfiLayoutTreeUpdateMark;
use crate::css::css_enums::positioning;
use crate::css::style::tree::StyleNodeID;
use crate::layout::FfiDisplay;
use crate::painting::paint_read::PaintRead;
use crate::render_state::{ArenaChange, DocumentHost};

/// Which level the box that leaves is at: what its style says, or, for a style change that stopped generating the box
/// and has swapped its style already, the level the change names.
#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum FfiDetachedBoxLevel {
    // Only the host names a level, and only a removal names this one.
    #[allow(dead_code)]
    FromStyle,
    Block,
    AtomicInline,
}

/// A DOM sibling of the node whose box leaves, by its identity, 0 for none or a sibling without one, and whether it
/// stays a direct child of the parent's box where it has no box of its own: anything but a display: contents element.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiDetachedBoxSibling {
    pub style_node: u32,
    pub is_direct_without_box: bool,
}

/// What only the DOM knows of a node whose box leaves its parent's box.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiDetachedBoxFacts {
    /// The parent is the body element, out of whose formatting structure an absolutely positioned box always stays.
    pub parent_is_body: bool,
    pub previous_sibling: FfiDetachedBoxSibling,
    pub next_sibling: FfiDetachedBoxSibling,
    pub level: FfiDetachedBoxLevel,
}

/// A removal the host streamed: the box of the node `child` leaves the box of its parent `parent`.
pub(crate) struct BoxRemoval {
    parent: StyleNodeID,
    child: StyleNodeID,
    facts: FfiDetachedBoxFacts,
}

impl LayoutNodeArena {
    fn display_of(&self, row: NodeSlotId) -> FfiDisplay {
        self.node_style_if_live(row)
            .map_or_else(FfiDisplay::block, |style| style.display())
    }

    fn is_absolutely_positioned_box(&self, row: NodeSlotId) -> bool {
        node_kind_is_node_with_style(self.data(row).kind.get())
            && self
                .node_style_if_live(row)
                .is_some_and(|style| style.box_values().position == positioning::ABSOLUTE)
    }

    /// Whether the box `child` of the node `child_node` can leave the box `parent` of its DOM parent `parent_node`
    /// without the anonymous wrappers or the box levels around it changing: what would have to be rebuilt otherwise.
    fn can_detach_box_in_place(
        &self,
        parent_node: StyleNodeID,
        parent: NodeSlotId,
        child: NodeSlotId,
        facts: &FfiDetachedBoxFacts,
    ) -> bool {
        let child_data = self.data(child);
        if !node_kind_is_node_with_style(child_data.kind.get()) {
            return false;
        }

        // OPTIMIZATION: Absolutely positioned boxes do not participate in their DOM parent's inline or block formatting
        //               structure, even when they are attached to an ancestor containing block. Removing them cannot
        //               disturb anonymous wrappers or sibling box levels.
        if self.is_absolutely_positioned_box(child)
            && (facts.parent_is_body
                || self.dom_node_style_node(self.containing_block_by_walking_ancestors(child)) == Some(parent_node))
        {
            return true;
        }

        let child_style = self.node_style_if_live(child);
        if child_data.parent.get() != parent || node_facts::node_is_out_of_flow(child_data, child_style) {
            return false;
        }
        let parent_data = self.data(parent);
        if !node_kind_is_node_with_style(parent_data.kind.get()) {
            return false;
        }

        let is_direct_child = |sibling: FfiDetachedBoxSibling| {
            let row =
                StyleNodeID::from_raw(sibling.style_node).map_or(NodeSlotId::INVALID, |node| self.bound_row(node));
            if row.is_invalid() {
                return sibling.is_direct_without_box;
            }
            self.data(row).parent.get() == parent
        };
        if !is_direct_child(facts.previous_sibling) && !is_direct_child(facts.next_sibling) {
            return false;
        }

        let parent_display = self.display_of(parent);
        if parent_display.is_flex_inside() || parent_display.is_grid_inside() {
            return true;
        }

        let children_are_inline = has_flag(parent_data, NodeFlag::ChildrenAreInline);
        // Direct block children and in-flow atomic inline children can be detached without changing anonymous wrapper
        // structure. Other box kinds still rebuild the parent so tree fixup can reconstruct any affected wrappers.
        if (parent_display.is_flow_inside() || parent_display.is_flow_root_inside()) && !children_are_inline {
            let is_anonymous = |row: NodeSlotId| !row.is_invalid() && has_flag(self.data(row), NodeFlag::Anonymous);
            if is_anonymous(child_data.previous_sibling.get()) && is_anonymous(child_data.next_sibling.get()) {
                return false;
            }
            // Once only anonymous wrappers would remain, a full rebuild would place their inline content directly in the
            // parent instead.
            let mut an_anonymous_inline_wrapper_remains = false;
            let mut an_in_flow_block_level_sibling_remains = false;
            let mut sibling = parent_data.first_child.get();
            while !sibling.is_invalid() {
                let data = self.data(sibling);
                let is_out_of_flow = node_kind_is_node_with_style(data.kind.get())
                    && node_facts::node_is_out_of_flow(data, self.node_style_if_live(sibling));
                if sibling != child && !is_out_of_flow {
                    if has_flag(data, NodeFlag::Anonymous) && has_flag(data, NodeFlag::ChildrenAreInline) {
                        an_anonymous_inline_wrapper_remains = true;
                    } else {
                        an_in_flow_block_level_sibling_remains = true;
                    }
                }
                sibling = data.next_sibling.get();
            }
            if an_anonymous_inline_wrapper_remains && !an_in_flow_block_level_sibling_remains {
                return false;
            }
            return match facts.level {
                FfiDetachedBoxLevel::FromStyle => self.display_of(child).is_block_outside(),
                level => level == FfiDetachedBoxLevel::Block,
            };
        }
        if !children_are_inline {
            return false;
        }
        match facts.level {
            FfiDetachedBoxLevel::FromStyle => {
                let display = self.display_of(child);
                display.is_inline_outside() && display.is_flow_root_inside()
            }
            level => level == FfiDetachedBoxLevel::AtomicInline,
        }
    }
}

impl BoxRemoval {
    /// Detaches the removed node's box from its parent's box in place where it can, owing the host what dropping the
    /// subtree owes it, and marks the parent for the layout tree build to rebuild otherwise.
    pub(crate) fn apply(self, arena: *mut LayoutNodeArena, owed: &mut Vec<HostWorkDue>) {
        let Self { parent, child, facts } = self;
        // SAFETY: The render state holds the arena, and nothing else reaches it while the change applies.
        let arena_ref = unsafe { &*arena };
        let (parent_row, child_row) = (arena_ref.bound_row(parent), arena_ref.bound_row(child));
        if parent_row.is_invalid()
            || child_row.is_invalid()
            || !arena_ref.can_detach_box_in_place(parent, parent_row, child_row, &facts)
        {
            arena_ref.mark_layout_tree_update(Some(parent), FfiLayoutTreeUpdateMark::NODE_REMOVE);
            return;
        }

        let parent_contains_removed_abspos_box = arena_ref.is_absolutely_positioned_box(child_row)
            && arena_ref.containing_block_by_walking_ancestors(child_row) == parent_row;
        if parent_contains_removed_abspos_box {
            arena_ref.note_contained_abspos_child_removal(parent_row, child_row);
        }
        owed.push(
            LayoutWrite::PrepareSubtreeForRemoval { root: child_row }
                .apply(arena)
                .host_work,
        );
        let dropped = LayoutWrite::DropSubtree { root: child_row }.apply(arena);
        assert!(dropped.was_attached, "a box detached in place is its parent's child");
        owed.push(dropped.host_work);
        if arena_ref.data(parent_row).first_child.get().is_invalid() {
            arena_ref.set_node_flag(parent_row, NodeFlag::ChildrenAreInline, false);
        }
        // Nothing lays a contained absolutely positioned box out again: the host repaints for it.
        if !parent_contains_removed_abspos_box {
            arena_ref.set_needs_layout_update(parent_row, true);
        }
    }
}

/// Streams the removal of the element or text node `child` from its parent element `parent`, as of the host's writes so
/// far: the render state detaches the node's box from the parent's box in place, or marks the parent for the next
/// layout tree build to rebuild, as it finds the boxes. What the drop owes the host is paid once the job that applies
/// it is done.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_remove_box(
    host: *const DocumentHost,
    parent: u32,
    child: u32,
    facts: FfiDetachedBoxFacts,
) {
    assert!(!host.is_null(), "document host is null");
    let (Some(parent), Some(child)) = (StyleNodeID::from_raw(parent), StyleNodeID::from_raw(child)) else {
        panic!("a removed box and its parent are named by their identities");
    };
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { &*host };
    // The rows stop reading as the arena once the box leaves, so the host reads none of them again, and the drop writes
    // the chunks in place rather than copies of those the host would still share.
    host.let_go_of_rows();
    host.queue_change(ArenaChange::RemoveBox(BoxRemoval { parent, child, facts }));
}

/// Whether the box of the element `child` can leave the box of its parent `parent` in place, as a removal would find
/// it, spending `read`.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_can_detach_box_in_place(
    host: *const DocumentHost,
    read: &crate::render_state::BegunRead,
    parent: u32,
    child: u32,
    facts: FfiDetachedBoxFacts,
) -> bool {
    let (Some(parent), Some(child)) = (StyleNodeID::from_raw(parent), StyleNodeID::from_raw(child)) else {
        return false;
    };
    // SAFETY: Guaranteed by the caller.
    unsafe {
        super::shell_reads::read_arena(host, read, (parent, child, facts), |arena, (parent, child, facts)| {
            let (parent_row, child_row) = (arena.bound_row(parent), arena.bound_row(child));
            !parent_row.is_invalid()
                && !child_row.is_invalid()
                && arena.can_detach_box_in_place(parent, parent_row, child_row, &facts)
        })
    }
}
