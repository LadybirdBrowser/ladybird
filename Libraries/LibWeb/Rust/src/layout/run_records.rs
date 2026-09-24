/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use std::mem::MaybeUninit;
use std::ptr::NonNull;

/// The per-run registry of UsedValues records, backed by the slot-indexed side
/// table in the layout node arena. A run holds its root's record itself and
/// allocates every other record on the arena's record stack, which releases them
/// when the run returns. The scoped constructors lend records to the run, so
/// callers cannot extend the registry's lifetime.
pub(crate) struct RunRecords<'arena> {
    root: Node,
    root_used: Option<&'arena UsedValues>,
    arena: &'arena LayoutNodeArena,
    nonce: u64,
    stack_length_at_start: usize,
    records_outside_table: RefCell<HashMap<Node, NonNull<UsedValues>>>,
    // Wrapper sizing hands this state to the wrapper's child run, which consumes it while laying
    // out the table box. Keeping it on the run prevents measurement state from escaping a pass.
    table_inline_layouts: RefCell<HashMap<Node, table_formatting_context::TableInlineLayout>>,
    // A paragraph measured from its items leaves block sizes and baselines unresolved.
    omitted_line_layout: Cell<bool>,
}

struct InnermostRunGuard<'arena> {
    arena: &'arena LayoutNodeArena,
    previous: (Node, Node),
}

impl Drop for InnermostRunGuard<'_> {
    fn drop(&mut self) {
        self.arena.innermost_run.set(self.previous);
    }
}

pub(crate) enum RunRecordClaim {
    Claimed,
    AlreadyClaimed,
    HeldByEnclosingRun,
}

impl<'arena> RunRecords<'arena> {
    pub(crate) fn with_root<R>(
        arena: &'arena LayoutNodeArena,
        root: Node,
        root_containing_block: Node,
        root_used: &'arena UsedValues,
        run: impl FnOnce(&Self) -> R,
    ) -> R {
        Self::enter(arena, root, root_containing_block, Some(root_used), run)
    }

    pub(crate) fn with_unrooted<R>(
        arena: &'arena LayoutNodeArena,
        root: Node,
        root_containing_block: Node,
        run: impl FnOnce(&Self) -> R,
    ) -> R {
        Self::enter(arena, root, root_containing_block, None, run)
    }

    fn enter<R>(
        arena: &'arena LayoutNodeArena,
        root: Node,
        root_containing_block: Node,
        root_used: Option<&'arena UsedValues>,
        run: impl FnOnce(&Self) -> R,
    ) -> R {
        let _read_scope = arena.enter_read_scope(root);
        let _innermost_run = InnermostRunGuard {
            arena,
            previous: arena.innermost_run.replace((root, root_containing_block)),
        };
        let records = Self {
            root,
            root_used,
            arena,
            nonce: arena.begin_run(),
            stack_length_at_start: arena.run_record_stack.length(),
            records_outside_table: RefCell::new(HashMap::default()),
            table_inline_layouts: RefCell::new(HashMap::default()),
            omitted_line_layout: Cell::new(false),
        };
        run(&records)
    }

    pub(crate) fn note_omitted_line_layout(&self) {
        self.omitted_line_layout.set(true);
    }

    pub(crate) fn omitted_line_layout(&self) -> bool {
        self.omitted_line_layout.get()
    }

    pub(crate) fn register(&self, node: Node, used: UsedValues) -> &UsedValues {
        let slot_index = node.slot_index();
        let registered_twice = || -> ! {
            panic!(
                "slot {} registered twice in the run rooted at slot {}",
                slot_index,
                self.root.slot_index()
            )
        };
        if node == self.root && self.root_used.is_some() {
            registered_twice();
        }
        assert_eq!(
            self.arena.innermost_run_nonce(),
            Some(self.nonce),
            "only the innermost layout run may register records"
        );
        let record = self.arena.run_record_stack.push(used);
        match self.arena.claim_run_record(slot_index, self.nonce, record) {
            RunRecordClaim::Claimed => {}
            RunRecordClaim::AlreadyClaimed => registered_twice(),
            RunRecordClaim::HeldByEnclosingRun => {
                if self.records_outside_table.borrow_mut().insert(node, record).is_some() {
                    registered_twice();
                }
            }
        }
        // SAFETY: The record stays on the stack until this run returns, which ends this borrow of the run.
        unsafe { record.as_ref() }
    }

    pub(crate) fn create_used_values(
        &self,
        callbacks: &LayoutPass<'_>,
        node: Node,
        constraints: ContainingBlockConstraints,
    ) -> &UsedValues {
        self.register(node, used_values::create_used_values(callbacks, node, constraints))
    }

    #[track_caller]
    pub(crate) fn used_values(&self, node: Node) -> &UsedValues {
        let caller = std::panic::Location::caller();
        self.used_values_if_owned(node).unwrap_or_else(|| {
            panic!(
                "the run rooted at slot {} does not own the record for slot {} (read at {caller})",
                self.root.slot_index(),
                node.slot_index(),
            )
        })
    }

    pub(crate) fn used_values_if_owned(&self, node: Node) -> Option<&UsedValues> {
        if node == self.root
            && let Some(root_used) = self.root_used
        {
            return Some(root_used);
        }
        let record = self.arena.run_record(node.slot_index(), self.nonce).or_else(|| {
            let records = self.records_outside_table.borrow();
            if records.is_empty() {
                return None;
            }
            records.get(&node).copied()
        })?;
        // SAFETY: This run registered the record, and it stays on the stack until this run returns.
        Some(unsafe { record.as_ref() })
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
        // SAFETY: Only the innermost run registers records, so every record above this run's start is its own.
        //         References to them borrow this run, which is being dropped.
        unsafe { self.arena.run_record_stack.truncate(self.stack_length_at_start) };
        self.arena.end_run(self.nonce);
    }
}

const RECORDS_PER_STACK_CHUNK: usize = 64;

#[derive(Default)]
pub(crate) struct RunRecordStack {
    chunks: RefCell<Vec<NonNull<UsedValues>>>,
    length: Cell<usize>,
}

impl RunRecordStack {
    fn length(&self) -> usize {
        self.length.get()
    }

    fn push(&self, record: UsedValues) -> NonNull<UsedValues> {
        let index = self.length.get();
        let chunk_index = index / RECORDS_PER_STACK_CHUNK;
        let mut chunks = self.chunks.borrow_mut();
        if chunk_index == chunks.len() {
            let chunk = Box::<[UsedValues]>::new_uninit_slice(RECORDS_PER_STACK_CHUNK);
            chunks.push(NonNull::from(Box::leak(chunk)).cast());
        }
        // SAFETY: The slot lies inside the chunk and holds no record, since it is at the stack's length.
        let slot = unsafe {
            let slot = chunks[chunk_index].add(index % RECORDS_PER_STACK_CHUNK);
            slot.write(record);
            slot
        };
        self.length.set(index + 1);
        slot
    }

    /// # Safety
    ///
    /// No reference to a record at or above `length` may be live.
    unsafe fn truncate(&self, length: usize) {
        let chunks = self.chunks.borrow();
        while self.length.get() > length {
            let index = self.length.get() - 1;
            self.length.set(index);
            // SAFETY: The slot holds a record that nothing references, per this function's contract.
            unsafe {
                chunks[index / RECORDS_PER_STACK_CHUNK]
                    .add(index % RECORDS_PER_STACK_CHUNK)
                    .drop_in_place();
            }
        }
    }

    pub(crate) fn release_spare_chunks(&self) {
        assert_eq!(self.length.get(), 0, "layout run records outlived their pass");
        let mut chunks = self.chunks.borrow_mut();
        while chunks.len() > 1 {
            let chunk = chunks.pop().expect("the stack holds more than one chunk");
            // SAFETY: The stack is empty, so the chunk holds no records.
            unsafe { free_stack_chunk(chunk) };
        }
    }
}

impl Drop for RunRecordStack {
    fn drop(&mut self) {
        // SAFETY: Records borrow the runs that registered them, and no run outlives the arena.
        unsafe { self.truncate(0) };
        for chunk in self.chunks.get_mut().drain(..) {
            // SAFETY: The stack is empty, so the chunk holds no records.
            unsafe { free_stack_chunk(chunk) };
        }
    }
}

/// # Safety
///
/// `chunk` must come from `RunRecordStack::push` and hold no records.
unsafe fn free_stack_chunk(chunk: NonNull<UsedValues>) {
    let slots = std::ptr::slice_from_raw_parts_mut(
        chunk.as_ptr().cast::<MaybeUninit<UsedValues>>(),
        RECORDS_PER_STACK_CHUNK,
    );
    // SAFETY: The slots were allocated as a boxed slice of this length, per this function's contract.
    drop(unsafe { Box::from_raw(slots) });
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::rc::Rc;

    #[test]
    fn nested_runs_leave_their_parents_records_in_place() {
        let mut arena = LayoutNodeArena::new();
        let root = arena.allocate_for_test().slot;
        let child = arena.allocate_for_test().slot;
        let root_used = UsedValues::default();
        let nested_used = UsedValues::default();
        let parent_nonce = RunRecords::with_root(&arena, root, NodeSlotId::INVALID, &root_used, |parent| {
            let child_used: *const UsedValues = parent.register(child, UsedValues::default());
            RunRecords::with_root(&arena, child, root, &nested_used, |nested| {
                assert!(std::ptr::eq(parent.used_values(child), child_used));
                assert!(std::ptr::eq(parent.used_values(root), &raw const root_used));
                assert!(std::ptr::eq(nested.used_values(child), &raw const nested_used));
                assert!(nested.used_values_if_owned(root).is_none());
                RunRecords::with_unrooted(&arena, child, root, |measurement| {
                    let measured: *const UsedValues = measurement.register(child, UsedValues::default());
                    assert!(std::ptr::eq(measurement.used_values(child), measured));
                    assert!(std::ptr::eq(parent.used_values(child), child_used));
                    assert!(std::ptr::eq(nested.used_values(child), &raw const nested_used));
                });
                assert!(std::ptr::eq(parent.used_values(child), child_used));
            });
            assert!(std::ptr::eq(parent.used_values(child), child_used));
            parent.nonce
        });
        assert!(arena.run_record(root.slot_index(), parent_nonce).is_none());
        assert_eq!(arena.run_record_stack.length(), 0);
    }

    #[test]
    fn returning_runs_release_their_records_and_reuse_the_storage() {
        let mut arena = LayoutNodeArena::new();
        let root = arena.allocate_for_test().slot;
        let child = arena.allocate_for_test().slot;
        let lines = Rc::new(crate::layout::inline_content::InlineContent::default());
        let weak_lines = Rc::downgrade(&lines);
        let root_used = UsedValues::default();
        let first_record = RunRecords::with_root(&arena, root, NodeSlotId::INVALID, &root_used, |records| {
            let used = records.register(child, UsedValues::default());
            used.set_finished_line_data(lines);
            std::ptr::from_ref(used)
        });
        assert!(weak_lines.upgrade().is_none());
        let second_record = RunRecords::with_root(&arena, root, NodeSlotId::INVALID, &root_used, |records| {
            std::ptr::from_ref(records.register(child, UsedValues::default()))
        });
        assert_eq!(first_record, second_record);

        let nodes: Vec<_> = (0..RECORDS_PER_STACK_CHUNK * 2 + 1)
            .map(|_| arena.allocate_for_test().slot)
            .collect();
        RunRecords::with_unrooted(&arena, root, NodeSlotId::INVALID, |records| {
            for node in nodes {
                records.register(node, UsedValues::default());
            }
        });
        assert_eq!(arena.run_record_stack.chunks.borrow().len(), 3);
        arena.end_layout_pass();
        assert_eq!(arena.run_record_stack.chunks.borrow().len(), 1);
    }

    #[test]
    fn a_box_registered_by_a_returned_run_can_be_freed() {
        let mut arena = LayoutNodeArena::new();
        let root = arena.allocate_for_test().slot;
        let child = arena.allocate_for_test().slot;
        let root_used = UsedValues::default();
        RunRecords::with_root(&arena, root, NodeSlotId::INVALID, &root_used, |records| {
            records.register(child, UsedValues::default());
        });
        let _ = arena.free_subtree(child);
    }

    #[test]
    #[should_panic(expected = "registered twice")]
    fn registering_the_root_of_a_rooted_run_panics() {
        let mut arena = LayoutNodeArena::new();
        let root = arena.allocate_for_test().slot;
        let root_used = UsedValues::default();
        RunRecords::with_root(&arena, root, NodeSlotId::INVALID, &root_used, |records| {
            records.register(root, UsedValues::default());
        });
    }

    #[test]
    #[should_panic(expected = "only the innermost layout run may register records")]
    fn an_enclosing_run_cannot_register_while_a_nested_run_is_in_progress() {
        let mut arena = LayoutNodeArena::new();
        let root = arena.allocate_for_test().slot;
        let child = arena.allocate_for_test().slot;
        let root_used = UsedValues::default();
        RunRecords::with_unrooted(&arena, root, NodeSlotId::INVALID, |parent| {
            RunRecords::with_root(&arena, root, NodeSlotId::INVALID, &root_used, |_| {
                parent.register(child, UsedValues::default());
            });
        });
    }

    #[test]
    fn unwinding_a_nested_run_restores_its_parent() {
        let mut arena = LayoutNodeArena::new();
        let root = arena.allocate_for_test().slot;
        let child = arena.allocate_for_test().slot;
        let root_used = UsedValues::default();
        let nested_used = UsedValues::default();
        RunRecords::with_root(&arena, root, NodeSlotId::INVALID, &root_used, |parent| {
            let child_used: *const UsedValues = parent.register(child, UsedValues::default());
            let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                RunRecords::with_root(&arena, child, root, &nested_used, |nested| {
                    nested.register(root, UsedValues::default());
                    panic!("abort the nested measurement");
                });
            }));
            assert!(result.is_err());
            assert!(std::ptr::eq(parent.used_values(root), &raw const root_used));
            assert!(std::ptr::eq(parent.used_values(child), child_used));
        });
    }
}
