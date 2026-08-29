/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

/// The per-run registry of UsedValues records, backed by the slot-indexed side
/// table in the layout node arena. Registering a slot that a surrounding run
/// owns displaces that run's entry, and dropping the RunRecords restores it.
/// That is sound only because runs strictly nest on the call stack, so an
/// Rc<RunRecords> must never escape its run.
pub(crate) struct RunRecords {
    root: Node,
    arena: *mut c_void,
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

impl RunRecords {
    pub(crate) fn new(arena: *mut c_void, root: Node, root_used: std::rc::Rc<UsedValues>) -> Self {
        let records = Self::new_unrooted(arena, root);
        records.register(root, root_used);
        records
    }

    pub(crate) fn new_unrooted(arena: *mut c_void, root: Node) -> Self {
        // SAFETY: Layout passes borrow the document's arena synchronously, and
        // the document keeps it alive for the duration of the pass.
        let nonce = unsafe { LayoutNodeArena::from_handle(arena) }.allocate_run_nonce();
        Self {
            root,
            arena,
            nonce,
            undo: RefCell::new(Vec::new()),
            table_inline_layouts: RefCell::new(HashMap::default()),
        }
    }

    fn arena(&self) -> &LayoutNodeArena {
        // SAFETY: See new_unrooted().
        unsafe { LayoutNodeArena::from_handle(self.arena) }
    }

    pub(crate) fn register(&self, node: Node, used: std::rc::Rc<UsedValues>) {
        let slot_index = node.slot_index();
        let previous = self.arena().replace_run_record(slot_index, self.nonce, used);
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
        callbacks: &FfiLayoutFcCallbacks,
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
        self.arena().run_record(node.slot_index(), self.nonce)
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

impl Drop for RunRecords {
    fn drop(&mut self) {
        let undo = std::mem::take(self.undo.get_mut());
        let arena = self.arena();
        for entry in undo.into_iter().rev() {
            arena.restore_run_record(entry.slot_index, self.nonce, entry.previous);
        }
    }
}
