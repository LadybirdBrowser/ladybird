/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::LayoutNodeArena;
use crate::layout::layout_node_arena::FreedSubtree;
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::painting::paintable_rows::PaintableRowReset;
use crate::stage::MainThread;
use std::cell::RefCell;

mod host_calls;

pub(crate) use host_calls::{
    destroy_image_observers, destroy_owned_image_provider, destroy_shell, notify_owned_image_provider_of_detach,
};

/// Who answers the host calls a change to the layout tree makes: the host at once, which only a
/// main thread caller can ask, or the host work of the tree build that makes the change, which the
/// build's entry applies once the walk is over. A tree build walk holds no main thread token, so it
/// can only queue.
#[derive(Clone, Copy)]
pub(crate) enum HostCalls<'a> {
    Now(&'a MainThread<'a>),
    AfterTreeBuild(&'a TreeBuildHostWork),
}

/// One host call a tree build owes for a change it made to the layout tree.
enum OwedHostCall {
    /// A row left the layout tree: the image observers it holds are dropped, and the image
    /// provider it owns is told.
    RowDetached {
        row: NodeSlotId,
        kind: NodeKind,
    },
    /// What a freed subtree's rows held that the host owns the memory of.
    Freed(FreedSubtree),
    PaintableRowReset(PaintableRowReset),
    /// A kept box, whose row's style changed. Its layout node, if something made one, hears the
    /// style the row has once the walk is over, and nothing if the row has gone by then.
    ShellStyleChanged {
        row: NodeSlotId,
        attach_resources: bool,
    },
}

/// What a tree build owes the host, gathered on the walk's stack while the walk runs and applied
/// by the build's entry once it is over, in the order the walk came to owe it.
#[must_use = "a tree build's host work is applied once the walk is over"]
#[derive(Default)]
pub(crate) struct TreeBuildHostWork {
    owed: RefCell<Vec<OwedHostCall>>,
}

impl TreeBuildHostWork {
    fn owe(&self, call: OwedHostCall) {
        self.owed.borrow_mut().push(call);
    }

    /// Makes the host calls the walk owes, in the order it came to owe them, after telling the host
    /// which nodes gained or lost a box. A row the build freed again, such as whitespace table
    /// fixup removed, is owed no style change.
    pub(crate) fn apply(self, main_thread: &MainThread, arena: &LayoutNodeArena) {
        arena.pay_box_presence_queued_by_tree_build(main_thread);
        for call in self.owed.into_inner() {
            match call {
                OwedHostCall::RowDetached { row, kind } => {
                    crate::layout::layout_node_arena::tell_host_of_row_detach(main_thread, row, kind);
                }
                OwedHostCall::Freed(freed) => freed.destroy_shells_and_invoke_callbacks(main_thread),
                OwedHostCall::PaintableRowReset(reset) => reset.tell(main_thread),
                OwedHostCall::ShellStyleChanged { row, attach_resources } => {
                    arena.tell_shell_of_style_change(main_thread, row, attach_resources);
                }
            }
        }
    }
}

impl HostCalls<'_> {
    /// Frees the subtree `root` heads, and destroys what its rows held that the host owns the
    /// memory of: at once, or once the tree build is over.
    pub(crate) fn free_subtree(self, arena: *mut LayoutNodeArena, root: NodeSlotId) {
        // SAFETY: Callers hold no reference derived from the arena across this call, and the
        // mutable borrow ends before the shells are destroyed.
        let freed = unsafe { &mut *arena }.free_subtree(root);
        match self {
            HostCalls::Now(main_thread) => freed.destroy_shells_and_invoke_callbacks(main_thread),
            HostCalls::AfterTreeBuild(work) => work.owe(OwedHostCall::Freed(freed)),
        }
    }

    /// Tells the document's chrome state that a row's paint state was reset.
    pub(crate) fn paintable_row_reset(self, reset: PaintableRowReset) {
        match self {
            HostCalls::Now(main_thread) => reset.tell(main_thread),
            HostCalls::AfterTreeBuild(work) => work.owe(OwedHostCall::PaintableRowReset(reset)),
        }
    }

    /// Drops the image observers a row leaving the layout tree holds, and tells the image provider
    /// it owns.
    pub(crate) fn row_detached(self, row: NodeSlotId, kind: NodeKind) {
        match self {
            HostCalls::Now(main_thread) => {
                crate::layout::layout_node_arena::tell_host_of_row_detach(main_thread, row, kind);
            }
            HostCalls::AfterTreeBuild(work) => work.owe(OwedHostCall::RowDetached { row, kind }),
        }
    }

    /// Tells the layout node of a row whose style changed, if something made one.
    pub(crate) fn shell_style_changed(self, arena: &LayoutNodeArena, row: NodeSlotId, attach_resources: bool) {
        match self {
            HostCalls::Now(main_thread) => arena.tell_shell_of_style_change(main_thread, row, attach_resources),
            HostCalls::AfterTreeBuild(work) => work.owe(OwedHostCall::ShellStyleChanged { row, attach_resources }),
        }
    }
}

pub(crate) fn free_subtree_and_destroy_shells(main_thread: &MainThread, arena: *mut LayoutNodeArena, root: NodeSlotId) {
    HostCalls::Now(main_thread).free_subtree(arena, root);
}

#[must_use = "an unplaced layout node must be attached or freed"]
pub(crate) struct UnplacedLayoutNode(NodeSlotId);

impl UnplacedLayoutNode {
    pub(crate) fn new(slot: NodeSlotId) -> Self {
        assert!(!slot.is_invalid(), "an unplaced layout node needs a live slot");
        Self(slot)
    }

    pub(crate) fn slot(&self) -> NodeSlotId {
        self.0
    }

    pub(crate) fn into_slot(self) -> NodeSlotId {
        let slot = self.0;
        std::mem::forget(self);
        slot
    }

    pub(crate) fn placed_as_layout_root(self) {
        self.into_slot();
    }
}

impl Drop for UnplacedLayoutNode {
    fn drop(&mut self) {
        debug_assert!(
            std::thread::panicking(),
            "unplaced layout node {:?} leaked without being attached or freed",
            self.0
        );
    }
}

fn parent_of(arena: &LayoutNodeArena, node: NodeSlotId) -> NodeSlotId {
    arena.data(node).parent.get()
}

fn next_sibling_of(arena: &LayoutNodeArena, node: NodeSlotId) -> NodeSlotId {
    arena.data(node).next_sibling.get()
}

impl LayoutNodeArena {
    pub(crate) fn attach_child(&self, parent: NodeSlotId, child: UnplacedLayoutNode, before: NodeSlotId) {
        self.assert_owner_thread();
        self.insert_child(parent, child.into_slot(), before);
    }

    pub(crate) fn detach_child(&self, parent: NodeSlotId, child: NodeSlotId) {
        self.assert_owner_thread();
        self.remove_child(parent, child);
    }

    pub(crate) fn detach_from_parent(&self, node: NodeSlotId) -> bool {
        let parent = parent_of(self, node);
        if parent.is_invalid() {
            return false;
        }
        self.detach_child(parent, node);
        true
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
        new_child: UnplacedLayoutNode,
    ) -> NodeSlotId {
        let successor = next_sibling_of(self, old_child);
        self.detach_child(parent, old_child);
        self.attach_child(parent, new_child, successor);
        old_child
    }
}

#[cfg(test)]
mod ffi_test_stubs {
    #[unsafe(no_mangle)]
    extern "C" fn ladybird_layout_node_shell_destroy(_shell: *mut std::ffi::c_void) {}

    #[unsafe(no_mangle)]
    extern "C" fn ladybird_layout_owned_image_provider_destroy(_provider: *mut std::ffi::c_void) {}

    #[unsafe(no_mangle)]
    extern "C" fn ladybird_layout_image_observers_destroy(_observers: *mut std::ffi::c_void) {}

    #[unsafe(no_mangle)]
    extern "C" fn ladybird_layout_owned_image_provider_notify_detach(_provider: *mut std::ffi::c_void) {}
}

#[cfg(test)]
mod tests {
    use super::UnplacedLayoutNode;
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
        let data = arena.data(node);
        Links {
            parent: data.parent.get(),
            first_child: data.first_child.get(),
            last_child: data.last_child.get(),
            previous_sibling: data.previous_sibling.get(),
            next_sibling: data.next_sibling.get(),
        }
    }

    fn fragment_cache_epoch(arena: &LayoutNodeArena, node: NodeSlotId) -> u32 {
        arena.data(node).fragment_cache_epoch.get()
    }

    fn owned(slot: NodeSlotId) -> UnplacedLayoutNode {
        UnplacedLayoutNode::new(slot)
    }

    fn free(arena: &mut LayoutNodeArena, allocation: NodeAllocation) {
        arena
            .free_subtree(allocation.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
    }

    #[test]
    fn attaching_an_owned_child_and_detaching_it_round_trips_the_links() {
        let mut arena = LayoutNodeArena::new();
        let parent = arena.allocate_for_test();
        let child = arena.allocate_for_test();

        arena.attach_child(parent.slot, owned(child.slot), NodeSlotId::INVALID);
        assert_eq!(links(&arena, parent.slot).first_child, child.slot);
        assert_eq!(links(&arena, parent.slot).last_child, child.slot);
        assert_eq!(links(&arena, child.slot).parent, parent.slot);

        arena.detach_child(parent.slot, child.slot);
        assert!(links(&arena, parent.slot).first_child.is_invalid());
        assert!(links(&arena, child.slot).parent.is_invalid());

        free(&mut arena, parent);
        free(&mut arena, child);
    }

    #[test]
    fn detaching_from_the_parent_unlinks_an_attached_child_and_skips_a_root() {
        let mut arena = LayoutNodeArena::new();
        let parent = arena.allocate_for_test();
        let child = arena.allocate_for_test();
        arena.attach_child(parent.slot, owned(child.slot), NodeSlotId::INVALID);

        assert!(arena.detach_from_parent(child.slot));
        assert!(links(&arena, parent.slot).first_child.is_invalid());
        assert!(!arena.detach_from_parent(parent.slot));

        free(&mut arena, parent);
        free(&mut arena, child);
    }

    #[test]
    fn replacing_a_child_keeps_the_sibling_position() {
        let mut arena = LayoutNodeArena::new();
        let parent = arena.allocate_for_test();
        let a = arena.allocate_for_test();
        let b = arena.allocate_for_test();
        let c = arena.allocate_for_test();
        let d = arena.allocate_for_test();
        for child in [a.slot, b.slot, c.slot] {
            arena.attach_child(parent.slot, owned(child), NodeSlotId::INVALID);
        }

        assert_eq!(arena.replace_child(parent.slot, b.slot, owned(d.slot)), b.slot);

        assert_eq!(links(&arena, parent.slot).first_child, a.slot);
        assert_eq!(links(&arena, a.slot).next_sibling, d.slot);
        assert_eq!(links(&arena, d.slot).previous_sibling, a.slot);
        assert_eq!(links(&arena, d.slot).next_sibling, c.slot);
        assert_eq!(links(&arena, c.slot).previous_sibling, d.slot);
        assert_eq!(links(&arena, parent.slot).last_child, c.slot);
        assert!(links(&arena, b.slot).parent.is_invalid());

        free(&mut arena, b);
        free(&mut arena, parent);
    }

    #[test]
    fn moving_a_child_relinks_it_under_the_new_parent() {
        let mut arena = LayoutNodeArena::new();
        let first_parent = arena.allocate_for_test();
        let second_parent = arena.allocate_for_test();
        let a = arena.allocate_for_test();
        let b = arena.allocate_for_test();
        let c = arena.allocate_for_test();
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
    }

    #[test]
    fn freeing_a_subtree_frees_every_descendant_in_one_call() {
        let mut arena = LayoutNodeArena::new();
        let root = arena.allocate_for_test();
        let a = arena.allocate_for_test();
        let b = arena.allocate_for_test();
        let c = arena.allocate_for_test();
        arena.attach_child(root.slot, owned(a.slot), NodeSlotId::INVALID);
        arena.attach_child(a.slot, owned(b.slot), NodeSlotId::INVALID);
        arena.attach_child(root.slot, owned(c.slot), NodeSlotId::INVALID);

        let freed = arena.free_subtree(root.slot);

        assert_eq!(freed.row_count(), 4);
        for slot in [root.slot, a.slot, b.slot, c.slot] {
            assert!(!arena.slot_is_live(slot));
        }
        freed.destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
    }

    #[test]
    #[should_panic(expected = "still linked under a parent")]
    fn freeing_a_subtree_whose_root_is_still_attached_panics() {
        let mut arena = LayoutNodeArena::new();
        let parent = arena.allocate_for_test();
        let child = arena.allocate_for_test();
        arena.attach_child(parent.slot, owned(child.slot), NodeSlotId::INVALID);
        let _ = arena.free_subtree(child.slot);
    }

    #[test]
    fn a_structural_change_bumps_the_fragment_cache_epoch_of_every_ancestor() {
        if super::super::fc_run_cache::fc_run_cache_mode_from_environment()
            == super::super::fc_run_cache::FcRunCacheMode::Disabled
        {
            return;
        }
        let mut arena = LayoutNodeArena::new();
        let grandparent = arena.allocate_for_test();
        let parent = arena.allocate_for_test();
        let sibling = arena.allocate_for_test();
        let child = arena.allocate_for_test();
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
    }

    #[test]
    fn a_structural_change_clears_the_overflow_validity_of_box_ancestors_only() {
        let mut arena = LayoutNodeArena::new();
        let grandparent = arena.allocate_for_test();
        let parent = arena.allocate_for_test();
        let child = arena.allocate_for_test();
        arena.data(grandparent.slot).kind.set(NodeKind::BlockContainer);
        arena.data(parent.slot).kind.set(NodeKind::InlineNode);
        arena.attach_child(grandparent.slot, owned(parent.slot), NodeSlotId::INVALID);
        for node in [grandparent.slot, parent.slot] {
            arena.populate_paintable_row(node);
            arena
                .paintable_side_data(node)
                .overflow_valid_across_recommits
                .set(true);
        }

        arena.attach_child(parent.slot, owned(child.slot), NodeSlotId::INVALID);

        assert!(
            !arena
                .paintable_side_data(grandparent.slot)
                .overflow_valid_across_recommits
                .get()
        );
        assert!(
            arena
                .paintable_side_data(parent.slot)
                .overflow_valid_across_recommits
                .get()
        );

        free(&mut arena, grandparent);
    }

    #[cfg(debug_assertions)]
    #[test]
    #[should_panic(expected = "leaked without being attached or freed")]
    fn dropping_an_unplaced_layout_node_panics() {
        let mut arena = LayoutNodeArena::new();
        let node = arena.allocate_for_test();
        let _ = UnplacedLayoutNode::new(node.slot);
    }
}
