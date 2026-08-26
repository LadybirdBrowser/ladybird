/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use std::ffi::c_void;

unsafe extern "C" {
    fn ladybird_layout_node_shell_release(shell: *mut c_void);
}

fn release_shell(shell: *mut c_void) {
    if shell.is_null() {
        debug_assert!(cfg!(test), "layout node has no C++ shell");
        return;
    }
    // SAFETY: Node::unref() may destroy the shell and re-enter layout_arena_free; every caller
    // releases with no arena borrow live.
    unsafe { ladybird_layout_node_shell_release(shell) };
}

#[must_use = "the tree's reference to the detached layout node must be released"]
pub(crate) struct DetachedShell(*mut c_void);

impl DetachedShell {
    pub(crate) fn from_tree_reference(shell: *mut c_void) -> Self {
        Self(shell)
    }

    pub(crate) fn release(self) {
        let shell = self.0;
        std::mem::forget(self);
        release_shell(shell);
    }
}

impl Drop for DetachedShell {
    fn drop(&mut self) {
        debug_assert!(
            std::thread::panicking(),
            "detached layout node shell dropped without being released"
        );
    }
}

#[must_use = "the tree's references to the detached layout nodes must be released"]
#[derive(Default)]
pub(crate) struct DetachedShells(Vec<*mut c_void>);

impl DetachedShells {
    pub(crate) fn push(&mut self, detached: DetachedShell) {
        let shell = detached.0;
        std::mem::forget(detached);
        self.0.push(shell);
    }

    #[cfg(test)]
    pub(crate) fn len(&self) -> usize {
        self.0.len()
    }

    pub(crate) fn release_all(mut self) {
        let shells = std::mem::take(&mut self.0);
        std::mem::forget(self);
        for shell in shells {
            release_shell(shell);
        }
    }
}

impl Drop for DetachedShells {
    fn drop(&mut self) {
        debug_assert!(
            self.0.is_empty() || std::thread::panicking(),
            "detached layout node shells dropped without being released"
        );
    }
}

#[must_use = "an owned layout node must be attached or released"]
pub(crate) struct OwnedLayoutNode(NodeSlotId);

impl OwnedLayoutNode {
    /// # Safety
    ///
    /// `slot` must name a live node whose shell carries exactly one reference that was handed
    /// to the caller.
    pub(crate) unsafe fn adopt_created(slot: NodeSlotId) -> Self {
        assert!(!slot.is_invalid(), "adopted an invalid layout node slot");
        Self(slot)
    }

    pub(crate) fn slot(&self) -> NodeSlotId {
        self.0
    }

    fn into_slot(self) -> NodeSlotId {
        let slot = self.0;
        std::mem::forget(self);
        slot
    }

    pub(crate) fn into_detached_shell(self, arena: &LayoutNodeArena) -> DetachedShell {
        DetachedShell(arena.node_shell(self.into_slot()))
    }
}

impl Drop for OwnedLayoutNode {
    fn drop(&mut self) {
        debug_assert!(
            std::thread::panicking(),
            "owned layout node {:?} leaked without being attached or released",
            self.0
        );
    }
}

fn parent_of(arena: &LayoutNodeArena, node: NodeSlotId) -> NodeSlotId {
    // SAFETY: data() validated that node names a live slot; the link is a plain value.
    unsafe { (&raw const (*arena.data(node)).parent).read() }
}

fn next_sibling_of(arena: &LayoutNodeArena, node: NodeSlotId) -> NodeSlotId {
    // SAFETY: data() validated that node names a live slot; the link is a plain value.
    unsafe { (&raw const (*arena.data(node)).next_sibling).read() }
}

impl LayoutNodeArena {
    pub(crate) fn attach_child(&self, parent: NodeSlotId, child: OwnedLayoutNode, before: NodeSlotId) {
        self.assert_owner_thread();
        self.insert_child(parent, child.into_slot(), before);
    }

    pub(crate) fn detach_child(&self, parent: NodeSlotId, child: NodeSlotId) -> DetachedShell {
        self.assert_owner_thread();
        let shell = self.node_shell(child);
        self.remove_child(parent, child);
        DetachedShell(shell)
    }

    pub(crate) fn detach_from_parent(&self, node: NodeSlotId) -> Option<DetachedShell> {
        let parent = parent_of(self, node);
        (!parent.is_invalid()).then(|| self.detach_child(parent, node))
    }

    pub(crate) fn move_child(&self, child: NodeSlotId, new_parent: NodeSlotId, before: NodeSlotId) {
        self.assert_owner_thread();
        let old_parent = parent_of(self, child);
        assert!(!old_parent.is_invalid(), "moved layout node has no parent");
        self.remove_child(old_parent, child);
        self.insert_child(new_parent, child, before);
    }

    pub(crate) fn replace_child(
        &self,
        parent: NodeSlotId,
        old_child: NodeSlotId,
        new_child: OwnedLayoutNode,
    ) -> DetachedShell {
        let successor = next_sibling_of(self, old_child);
        let detached = self.detach_child(parent, old_child);
        self.attach_child(parent, new_child, successor);
        detached
    }
}

#[cfg(test)]
mod ffi_test_stubs {
    #[unsafe(no_mangle)]
    extern "C" fn ladybird_layout_node_shell_release(_shell: *mut std::ffi::c_void) {}
}

#[cfg(test)]
mod tests {
    use super::OwnedLayoutNode;
    use crate::layout::LayoutNodeArena;
    use crate::layout::layout_node_arena::NodeAllocation;
    use crate::layout::node_data::{NodeKind, NodeSlotId};

    struct Links {
        parent: NodeSlotId,
        first_child: NodeSlotId,
        last_child: NodeSlotId,
        previous_sibling: NodeSlotId,
        next_sibling: NodeSlotId,
    }

    fn links(arena: &LayoutNodeArena, node: NodeSlotId) -> Links {
        // SAFETY: Every test keeps its allocations live while reading them.
        let data = unsafe { &*arena.data(node) };
        Links {
            parent: data.parent,
            first_child: data.first_child,
            last_child: data.last_child,
            previous_sibling: data.previous_sibling,
            next_sibling: data.next_sibling,
        }
    }

    fn fragment_cache_epoch(arena: &LayoutNodeArena, node: NodeSlotId) -> u32 {
        // SAFETY: Every test keeps its allocations live while reading them.
        unsafe { (*arena.data(node)).fragment_cache_epoch }
    }

    fn owned(slot: NodeSlotId) -> OwnedLayoutNode {
        // SAFETY: Test nodes have no shell, and the release hook skips null shells.
        unsafe { OwnedLayoutNode::adopt_created(slot) }
    }

    fn free(arena: &mut LayoutNodeArena, allocation: NodeAllocation) {
        arena
            .free(allocation.slot, allocation.generation)
            .detached_children
            .release_all();
    }

    #[test]
    fn attaching_an_owned_child_and_detaching_it_round_trips_the_links() {
        let mut arena = LayoutNodeArena::new();
        let parent = arena.allocate();
        let child = arena.allocate();

        arena.attach_child(parent.slot, owned(child.slot), NodeSlotId::INVALID);
        assert_eq!(links(&arena, parent.slot).first_child, child.slot);
        assert_eq!(links(&arena, parent.slot).last_child, child.slot);
        assert_eq!(links(&arena, child.slot).parent, parent.slot);

        arena.detach_child(parent.slot, child.slot).release();
        assert!(links(&arena, parent.slot).first_child.is_invalid());
        assert!(links(&arena, child.slot).parent.is_invalid());

        free(&mut arena, parent);
        free(&mut arena, child);
    }

    #[test]
    fn detaching_from_the_parent_unlinks_an_attached_child_and_skips_a_root() {
        let mut arena = LayoutNodeArena::new();
        let parent = arena.allocate();
        let child = arena.allocate();
        arena.attach_child(parent.slot, owned(child.slot), NodeSlotId::INVALID);

        arena.detach_from_parent(child.slot).expect("attached child").release();
        assert!(links(&arena, parent.slot).first_child.is_invalid());
        assert!(arena.detach_from_parent(parent.slot).is_none());

        free(&mut arena, parent);
        free(&mut arena, child);
    }

    #[test]
    fn replacing_a_child_keeps_the_sibling_position() {
        let mut arena = LayoutNodeArena::new();
        let parent = arena.allocate();
        let a = arena.allocate();
        let b = arena.allocate();
        let c = arena.allocate();
        let d = arena.allocate();
        for child in [a.slot, b.slot, c.slot] {
            arena.attach_child(parent.slot, owned(child), NodeSlotId::INVALID);
        }

        arena.replace_child(parent.slot, b.slot, owned(d.slot)).release();

        assert_eq!(links(&arena, parent.slot).first_child, a.slot);
        assert_eq!(links(&arena, a.slot).next_sibling, d.slot);
        assert_eq!(links(&arena, d.slot).previous_sibling, a.slot);
        assert_eq!(links(&arena, d.slot).next_sibling, c.slot);
        assert_eq!(links(&arena, c.slot).previous_sibling, d.slot);
        assert_eq!(links(&arena, parent.slot).last_child, c.slot);
        assert!(links(&arena, b.slot).parent.is_invalid());

        free(&mut arena, b);
        free(&mut arena, parent);
        for orphan in [a, d, c] {
            free(&mut arena, orphan);
        }
    }

    #[test]
    fn moving_a_child_relinks_it_under_the_new_parent() {
        let mut arena = LayoutNodeArena::new();
        let first_parent = arena.allocate();
        let second_parent = arena.allocate();
        let a = arena.allocate();
        let b = arena.allocate();
        let c = arena.allocate();
        arena.attach_child(first_parent.slot, owned(a.slot), NodeSlotId::INVALID);
        arena.attach_child(first_parent.slot, owned(b.slot), NodeSlotId::INVALID);
        arena.attach_child(second_parent.slot, owned(c.slot), NodeSlotId::INVALID);

        arena.move_child(b.slot, second_parent.slot, c.slot);

        assert_eq!(links(&arena, first_parent.slot).first_child, a.slot);
        assert_eq!(links(&arena, first_parent.slot).last_child, a.slot);
        assert_eq!(links(&arena, second_parent.slot).first_child, b.slot);
        assert_eq!(links(&arena, b.slot).next_sibling, c.slot);
        assert_eq!(links(&arena, b.slot).parent, second_parent.slot);

        free(&mut arena, first_parent);
        free(&mut arena, second_parent);
        for orphan in [a, b, c] {
            free(&mut arena, orphan);
        }
    }

    #[test]
    fn freeing_a_node_unlinks_its_children_and_hands_back_their_references() {
        let mut arena = LayoutNodeArena::new();
        let root = arena.allocate();
        let a = arena.allocate();
        let b = arena.allocate();
        arena.attach_child(root.slot, owned(a.slot), NodeSlotId::INVALID);
        arena.attach_child(root.slot, owned(b.slot), NodeSlotId::INVALID);

        let freed = arena.free(root.slot, root.generation);
        assert_eq!(freed.detached_children.len(), 2);
        assert!(links(&arena, a.slot).parent.is_invalid());
        assert!(links(&arena, b.slot).parent.is_invalid());
        assert!(links(&arena, a.slot).next_sibling.is_invalid());
        freed.detached_children.release_all();

        free(&mut arena, a);
        free(&mut arena, b);
    }

    #[test]
    #[should_panic(expected = "still linked under a parent")]
    fn freeing_a_node_still_linked_under_a_parent_panics() {
        let mut arena = LayoutNodeArena::new();
        let parent = arena.allocate();
        let child = arena.allocate();
        arena.attach_child(parent.slot, owned(child.slot), NodeSlotId::INVALID);
        let _ = arena.free(child.slot, child.generation);
    }

    #[test]
    fn a_structural_change_bumps_the_fragment_cache_epoch_of_every_ancestor() {
        if crate::layout::fc_run_cache_mode_from_environment() == crate::layout::FcRunCacheMode::Disabled {
            return;
        }
        let mut arena = LayoutNodeArena::new();
        let grandparent = arena.allocate();
        let parent = arena.allocate();
        let sibling = arena.allocate();
        let child = arena.allocate();
        arena.attach_child(grandparent.slot, owned(parent.slot), NodeSlotId::INVALID);
        arena.attach_child(grandparent.slot, owned(sibling.slot), NodeSlotId::INVALID);
        let grandparent_epoch = fragment_cache_epoch(&arena, grandparent.slot);
        let parent_epoch = fragment_cache_epoch(&arena, parent.slot);
        let sibling_epoch = fragment_cache_epoch(&arena, sibling.slot);

        arena.attach_child(parent.slot, owned(child.slot), NodeSlotId::INVALID);

        assert_eq!(fragment_cache_epoch(&arena, grandparent.slot), grandparent_epoch + 1);
        assert_eq!(fragment_cache_epoch(&arena, parent.slot), parent_epoch + 1);
        assert_eq!(fragment_cache_epoch(&arena, sibling.slot), sibling_epoch);
        assert_eq!(fragment_cache_epoch(&arena, child.slot), 0);

        free(&mut arena, grandparent);
        for orphan in [parent, sibling, child] {
            free(&mut arena, orphan);
        }
    }

    #[test]
    fn a_structural_change_clears_the_overflow_validity_of_box_ancestors_only() {
        let mut arena = LayoutNodeArena::new();
        let grandparent = arena.allocate();
        let parent = arena.allocate();
        let child = arena.allocate();
        // SAFETY: The allocations stay live for the whole test.
        unsafe {
            (*grandparent.data).kind = NodeKind::BlockContainer;
            (*parent.data).kind = NodeKind::InlineNode;
        }
        arena.attach_child(grandparent.slot, owned(parent.slot), NodeSlotId::INVALID);
        for node in [grandparent.slot, parent.slot] {
            arena.populate_paintable_row(node);
            arena
                .paintable_rows_mut()
                .paintable_data_mut(node)
                .overflow_valid_across_recommits = true;
        }

        arena.attach_child(parent.slot, owned(child.slot), NodeSlotId::INVALID);

        assert!(
            !arena
                .paintable_rows()
                .paintable_data(grandparent.slot)
                .overflow_valid_across_recommits
        );
        assert!(
            arena
                .paintable_rows()
                .paintable_data(parent.slot)
                .overflow_valid_across_recommits
        );

        free(&mut arena, grandparent);
        free(&mut arena, parent);
        free(&mut arena, child);
    }

    #[cfg(debug_assertions)]
    #[test]
    #[should_panic(expected = "without being released")]
    fn dropping_a_detached_shell_without_releasing_it_panics() {
        let mut arena = LayoutNodeArena::new();
        let parent = arena.allocate();
        let child = arena.allocate();
        arena.attach_child(parent.slot, owned(child.slot), NodeSlotId::INVALID);
        let _ = arena.detach_child(parent.slot, child.slot);
    }

    #[cfg(debug_assertions)]
    #[test]
    #[should_panic(expected = "leaked without being attached or released")]
    fn dropping_an_owned_layout_node_without_placing_it_panics() {
        let mut arena = LayoutNodeArena::new();
        let node = arena.allocate();
        // SAFETY: Test nodes have no shell, and the hooks skip null shells.
        let _ = unsafe { OwnedLayoutNode::adopt_created(node.slot) };
    }
}
