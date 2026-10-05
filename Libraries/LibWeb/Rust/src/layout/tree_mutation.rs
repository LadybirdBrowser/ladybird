/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::LayoutNodeArena;
use crate::layout::layout_node_arena::FreedSubtree;
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::painting::paint_read::GeometryRead;
use crate::painting::paintable_rows::PaintableRowReset;
use crate::stage::MainThread;
use std::cell::RefCell;

mod host_calls;

pub(crate) use host_calls::{
    destroy_image_observers, destroy_owned_image_provider, destroy_shell, notify_owned_image_provider_of_detach,
};

/// What the host calls a change to the layout tree makes go to: the host work the change owes, which its entry pays once
/// the change is over (see [`OwedHostWork::resolve`]). Nothing that changes the layout tree calls the host itself.
#[derive(Clone, Copy)]
pub(crate) struct HostCalls<'a>(pub(crate) &'a OwedHostWork);

/// One host call owed for a change made to the layout tree.
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

/// What a tree build or a layout write owes the host, gathered while it runs and resolved as it ends (see
/// [`HostWorkDue`]), in the order it came to owe it. Box presence is queued for as long.
#[must_use = "owed host work is applied once the change is over"]
#[derive(Default)]
pub(crate) struct OwedHostWork {
    owed: RefCell<Vec<OwedHostCall>>,
}

impl OwedHostWork {
    fn owe(&self, call: OwedHostCall) {
        self.owed.borrow_mut().push(call);
    }

    /// Resolves what the change owes the host against `arena`, the arena it changed, as it ends: which nodes gained
    /// or lost a box, and the style a row whose layout node hears of it has. The host pays it with nothing but its own
    /// tables. A row the change freed again, such as whitespace table fixup removed, is owed no style change.
    pub(crate) fn resolve(self, arena: &LayoutNodeArena) -> HostWorkDue {
        let box_presence = arena.take_queued_box_presence();
        let calls = self
            .owed
            .into_inner()
            .into_iter()
            .filter_map(|call| {
                Some(match call {
                    OwedHostCall::RowDetached { row, kind } => DueHostCall::RowDetached { row, kind },
                    OwedHostCall::Freed(freed) => DueHostCall::Freed(freed),
                    OwedHostCall::PaintableRowReset(reset) => DueHostCall::PaintableRowReset(reset),
                    OwedHostCall::ShellStyleChanged { row, attach_resources } => {
                        let kind = arena.node_kind_if_live(row)?;
                        DueHostCall::ShellStyleChanged {
                            facts: crate::layout::host_tables::ShellFacts { id: row, kind },
                            record: arena.node_style_record(row),
                            payloads: arena.data(row).style.get(),
                            derived: arena.node_style_record_is_derived(row),
                            attach_resources,
                        }
                    }
                })
            })
            .collect();
        HostWorkDue { box_presence, calls }
    }
}

/// One host call a change to the layout tree owes, with what the host learns from it.
enum DueHostCall {
    RowDetached {
        row: NodeSlotId,
        kind: NodeKind,
    },
    Freed(FreedSubtree),
    PaintableRowReset(PaintableRowReset),
    /// The layout node of the row, if something made one, hears the row's style record and payloads, and whether the
    /// arena derived the record.
    ShellStyleChanged {
        facts: crate::layout::host_tables::ShellFacts,
        record: u64,
        payloads: crate::layout::node_data::StylePayloadsRef,
        derived: bool,
        attach_resources: bool,
    },
}

/// What a change to the layout tree owes the host, resolved against the arena it changed as it ended (see
/// [`OwedHostWork::resolve`]), which the host pays on its thread with nothing but its own tables, in the order the
/// change came to owe it, after it hears which nodes gained or lost a box.
#[must_use = "what a change owes the host is paid once the change is over"]
#[derive(Default)]
pub(crate) struct HostWorkDue {
    box_presence: crate::layout::layout_node_arena::DueBoxPresence,
    calls: Vec<DueHostCall>,
}

impl HostWorkDue {
    /// Makes the host calls owed.
    pub(crate) fn pay(self, main_thread: &MainThread) {
        self.box_presence.tell(main_thread);
        for call in self.calls {
            match call {
                DueHostCall::RowDetached { row, kind } => {
                    crate::layout::layout_node_arena::tell_host_of_row_detach(main_thread, row, kind);
                }
                DueHostCall::Freed(freed) => freed.destroy_shells_and_invoke_callbacks(main_thread),
                DueHostCall::PaintableRowReset(reset) => reset.tell(main_thread),
                DueHostCall::ShellStyleChanged {
                    facts,
                    record,
                    payloads,
                    derived,
                    attach_resources,
                } => tell_shell_of_style_change(main_thread, facts, record, payloads, derived, attach_resources),
            }
        }
    }
}

/// Tells the layout node of the row `facts` describes of its new style, if something made one.
fn tell_shell_of_style_change(
    main_thread: &MainThread,
    facts: crate::layout::host_tables::ShellFacts,
    record: u64,
    payloads: crate::layout::node_data::StylePayloadsRef,
    derived: bool,
    attach_resources: bool,
) {
    let Some(host_tables) = main_thread.host_tables() else {
        return;
    };
    let Some((context, shell_style_changed)) = host_tables.shell_style_changed_host.get() else {
        return;
    };
    // A row owed its style resources gets its shell now: the shell is what loads them, and a shell made later by the
    // factory would not attach them.
    let shell = if attach_resources {
        if facts.kind == NodeKind::Unset {
            return;
        }
        std::ptr::NonNull::new(host_tables.shell_of(facts))
    } else {
        host_tables.shells.borrow().get(&facts.id).copied()
    };
    let Some(shell) = shell else {
        return;
    };
    // SAFETY: The engine and shell remain live. Native style-store mutation has finished before the host can reenter
    // Rust through its resource consumers.
    unsafe {
        shell_style_changed(
            context,
            shell.as_ptr(),
            record,
            payloads.as_ptr().cast(),
            derived,
            attach_resources,
        );
    };
}

impl HostCalls<'_> {
    /// Frees the subtree `root` heads, and owes the host the destruction of what its rows held that the host owns the
    /// memory of.
    pub(crate) fn free_subtree(self, arena: *mut LayoutNodeArena, root: NodeSlotId) {
        // SAFETY: Callers hold no reference derived from the arena across this call.
        let freed = unsafe { &mut *arena }.free_subtree(root);
        self.0.owe(OwedHostCall::Freed(freed));
    }

    /// Owes the document's chrome state the news that a row's paint state was reset.
    pub(crate) fn paintable_row_reset(self, reset: PaintableRowReset) {
        self.0.owe(OwedHostCall::PaintableRowReset(reset));
    }

    /// Owes dropping the image observers a row leaving the layout tree holds, and telling the image provider it owns.
    pub(crate) fn row_detached(self, row: NodeSlotId, kind: NodeKind) {
        self.0.owe(OwedHostCall::RowDetached { row, kind });
    }

    /// Owes the layout node of a row whose style changed, if something made one, its new style.
    pub(crate) fn shell_style_changed(self, row: NodeSlotId, attach_resources: bool) {
        self.0.owe(OwedHostCall::ShellStyleChanged { row, attach_resources });
    }
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
        self.insert_child(parent, child.into_slot(), before);
    }

    pub(crate) fn detach_child(&self, parent: NodeSlotId, child: NodeSlotId) {
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
        arena.write_shape(grandparent.slot).set_kind(NodeKind::BlockContainer);
        arena.write_shape(parent.slot).set_kind(NodeKind::InlineNode);
        arena.attach_child(grandparent.slot, owned(parent.slot), NodeSlotId::INVALID);
        for node in [grandparent.slot, parent.slot] {
            arena.populate_paintable_row(node);
            arena.committed_side_data_mut(node).overflow_valid_across_recommits = true;
        }

        arena.attach_child(parent.slot, owned(child.slot), NodeSlotId::INVALID);

        assert!(
            !arena
                .committed_side_data(grandparent.slot)
                .overflow_valid_across_recommits
        );
        assert!(arena.committed_side_data(parent.slot).overflow_valid_across_recommits);

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
