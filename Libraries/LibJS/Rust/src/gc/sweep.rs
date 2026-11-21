/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The sweep callback of a collection, which runs once the marks are final and before the cells that died are swept,
//! and has what holds cells without keeping them alive forget the ones that died.

use crate::gc::capi;
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, CellState, Gc};
use crate::layout::value::Value;
use crate::runtime::finalization_registry::FinalizationRegistry;

/// What code can do while the sweep callback of a collection runs: tell which cells died, which only means anything
/// then, and hand finalization registries over for cleanup. It gives no VM, as nothing may allocate meanwhile.
pub struct Sweep<'vm>(&'vm Vm);

impl<'vm> Sweep<'vm> {
    /// # Safety
    ///
    /// Only the VM's sweep callback makes one, while the marks of its collection are final and the cells that died are
    /// not swept yet.
    pub(crate) unsafe fn new(vm: &'vm Vm) -> Self {
        Self(vm)
    }

    /// Whether `cell`, which something held without keeping it alive, did not survive the collection.
    pub fn is_dead<T>(&self, cell: Gc<T>) -> bool {
        // SAFETY: Every cell starts with its header, and a cell that died is intact until the sweep that follows.
        let header = unsafe { &*cell.as_ptr().cast::<CellHeader>() };
        // SAFETY: The cell is a cell.
        header.state.get() != CellState::Live || !unsafe { capi::gc_cell_is_marked(cell.as_ptr().cast()) }
    }

    /// Whether `cell`, which a cache held without keeping it alive, did not survive the collection, or moves in it. A
    /// cache that keeps the address of a cell without visiting it forgets the cell either way, while what holds cells
    /// weakly by identity pins them as it is traced (see Visitor::pin()).
    pub fn is_dead_or_moving<T>(&self, cell: Gc<T>) -> bool {
        // SAFETY: The cell is a cell, intact until the sweep that follows.
        self.is_dead(cell) || unsafe { capi::gc_cell_will_move(cell.as_ptr().cast()) }
    }

    /// Whether the collection may move cells, which is when what holds cells without visiting their slots hands over
    /// the slots of those that survived (see record_slot()).
    pub fn moves_cells(&self) -> bool {
        // NB: The VM lets go of its heap before the collection that destroys the heap, which moves nothing.
        self.0
            .heap_if_held()
            .is_some_and(|heap| capi::gc_heap_is_moving_cells(heap.handle()))
    }

    /// Has the collection rewrite `slot`, which holds a cell that survived it, if the cell moves, as it rewrites the slots
    /// marking visited. For what holds cells without keeping them alive or in place, like the keys of a weak map, once
    /// it forgot the cells that died.
    ///
    /// # Safety
    ///
    /// Nothing may free, move or write the slot until the collection is over.
    pub unsafe fn record_slot<T>(&self, slot: &Gc<T>) {
        // SAFETY: The slot holds a pointer to a cell that survived, and the caller vouches for its memory.
        unsafe {
            capi::gc_heap_record_slot(
                self.0.heap().raw(),
                core::ptr::from_ref(slot).cast(),
                capi::GC_RECORDED_SLOT_CELL_POINTER,
            );
        }
    }

    /// Like record_slot(), for a slot that holds a value, which may be a cell that survived the collection.
    ///
    /// # Safety
    ///
    /// As for record_slot().
    pub unsafe fn record_value_slot(&self, slot: &Value) {
        // SAFETY: The slot holds a value, and the caller vouches for its memory.
        unsafe {
            capi::gc_heap_record_slot(
                self.0.heap().raw(),
                core::ptr::from_ref(slot).cast(),
                capi::GC_RECORDED_SLOT_VALUE,
            );
        }
    }

    /// Hands `finalization_registry` to HostEnqueueFinalizationRegistryCleanupJob once the collection is over.
    pub fn enqueue_finalization_registry_cleanup_job(&self, finalization_registry: Gc<FinalizationRegistry>) {
        self.0
            .enqueue_finalization_registry_cleanup_job_after_collection(self, finalization_registry);
    }
}
