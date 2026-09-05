/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

/// The per-run registry of UsedValues records, backed by the slot-indexed side
/// table in the layout node arena. Nested runs temporarily displace their
/// parents' entries. The scoped constructors lend records to the run and restore
/// the entries before returning, so callers cannot extend the registry's lifetime.
pub(crate) struct RunRecords<'arena> {
    pub(crate) omitted_line_layout: Cell<bool>,
    root: Node,
    arena: &'arena LayoutNodeArena,
    nonce: u64,
    undo: RefCell<Vec<UndoEntry>>,
    // Wrapper sizing hands this state to the wrapper's child run, which consumes it while laying
    // out the table box. Keeping it on the run prevents measurement state from escaping a pass.
    table_inline_layouts: RefCell<HashMap<Node, table_formatting_context::TableInlineLayout>>,
}

struct UndoEntry {
    slot_index: u32,
    previous: Option<(u64, std::rc::Rc<UsedValues>)>,
}

impl<'arena> RunRecords<'arena> {
    pub(crate) fn with_root<R>(
        arena: &'arena LayoutNodeArena,
        root: Node,
        root_used: std::rc::Rc<UsedValues>,
        run: impl FnOnce(&Self) -> R,
    ) -> R {
        Self::with_unrooted(arena, root, |records| {
            records.register(root, root_used);
            run(records)
        })
    }

    pub(crate) fn with_unrooted<R>(arena: &'arena LayoutNodeArena, root: Node, run: impl FnOnce(&Self) -> R) -> R {
        let records = Self {
            omitted_line_layout: Cell::new(false),
            root,
            arena,
            nonce: arena.allocate_run_nonce(),
            undo: RefCell::new(Vec::new()),
            table_inline_layouts: RefCell::new(HashMap::default()),
        };
        run(&records)
    }

    pub(crate) fn register(&self, node: Node, used: std::rc::Rc<UsedValues>) {
        let slot_index = node.slot_index();
        let previous = self.arena.replace_run_record(slot_index, self.nonce, used);
        assert!(
            previous.as_ref().is_none_or(|(nonce, _)| *nonce != self.nonce),
            "slot {} registered twice in the run rooted at slot {}",
            slot_index,
            self.root.slot_index()
        );
        self.undo.borrow_mut().push(UndoEntry { slot_index, previous });
    }

    pub(crate) fn create_used_values(
        &self,
        callbacks: &LayoutPass<'_>,
        node: Node,
        constraints: ContainingBlockConstraints,
    ) -> std::rc::Rc<UsedValues> {
        let used = used_values::create_used_values(callbacks, node, constraints);
        self.register(node, used.clone());
        used
    }

    #[track_caller]
    pub(crate) fn used_values(&self, node: Node) -> std::rc::Rc<UsedValues> {
        let caller = std::panic::Location::caller();
        self.used_values_if_owned(node).unwrap_or_else(|| {
            panic!(
                "the run rooted at slot {} does not own the record for slot {} (read at {caller})",
                self.root.slot_index(),
                node.slot_index(),
            )
        })
    }

    pub(crate) fn used_values_if_owned(&self, node: Node) -> Option<std::rc::Rc<UsedValues>> {
        self.arena.run_record(node.slot_index(), self.nonce)
    }

    pub(crate) fn root(&self) -> Node {
        self.root
    }

    pub(crate) fn store_table_inline_layout(&self, table: Node, layout: table_formatting_context::TableInlineLayout) {
        self.table_inline_layouts.borrow_mut().insert(table, layout);
    }

    pub(crate) fn take_table_inline_layout(&self, table: Node) -> Option<table_formatting_context::TableInlineLayout> {
        self.table_inline_layouts.borrow_mut().remove(&table)
    }
}

impl Drop for RunRecords<'_> {
    fn drop(&mut self) {
        let undo = std::mem::take(self.undo.get_mut());
        let arena = self.arena;
        for entry in undo.into_iter().rev() {
            arena.restore_run_record(entry.slot_index, self.nonce, entry.previous);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::rc::Rc;

    #[test]
    fn nested_runs_restore_displaced_records_before_the_parent_continues() {
        let mut arena = LayoutNodeArena::new();
        let root = arena.allocate_for_test().slot;
        let child = arena.allocate_for_test().slot;
        let root_used = Rc::new(UsedValues::default());
        let child_used = Rc::new(UsedValues::default());
        let parent_nonce = RunRecords::with_root(&arena, root, root_used.clone(), |parent| {
            parent.register(child, child_used.clone());
            let nested_used = Rc::new(UsedValues::default());
            RunRecords::with_root(&arena, child, nested_used.clone(), |nested| {
                assert!(parent.used_values_if_owned(child).is_none());
                assert!(Rc::ptr_eq(&parent.used_values(root), &root_used));
                assert!(Rc::ptr_eq(&nested.used_values(child), &nested_used));
                RunRecords::with_unrooted(&arena, child, |measurement| {
                    measurement.register(child, Rc::new(UsedValues::default()));
                    assert!(nested.used_values_if_owned(child).is_none());
                });
                assert!(Rc::ptr_eq(&nested.used_values(child), &nested_used));
            });
            assert!(Rc::ptr_eq(&parent.used_values(child), &child_used));
            parent.nonce
        });
        assert!(arena.run_record(root.slot_index(), parent_nonce).is_none());
        assert!(arena.run_record(child.slot_index(), parent_nonce).is_none());
        assert_eq!(Rc::strong_count(&root_used), 1);
        assert_eq!(Rc::strong_count(&child_used), 1);
    }

    #[test]
    fn unwinding_a_nested_run_restores_its_parent() {
        let mut arena = LayoutNodeArena::new();
        let root = arena.allocate_for_test().slot;
        let root_used = Rc::new(UsedValues::default());
        RunRecords::with_root(&arena, root, root_used.clone(), |parent| {
            let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                RunRecords::with_root(&arena, root, Rc::new(UsedValues::default()), |_| {
                    panic!("abort the nested measurement");
                });
            }));
            assert!(result.is_err());
            assert!(Rc::ptr_eq(&parent.used_values(root), &root_used));
        });
        assert_eq!(Rc::strong_count(&root_used), 1);
    }
}
