/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The sweep callback of a collection, which runs once the marks are final and before the cells that died are swept,
//! and has what holds cells without keeping them alive forget the ones that died.

use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, CellState, Gc};
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
        header.state.get() != CellState::Live || !header.mark.get()
    }

    /// Hands `finalization_registry` to HostEnqueueFinalizationRegistryCleanupJob once the collection is over.
    pub fn enqueue_finalization_registry_cleanup_job(&self, finalization_registry: Gc<FinalizationRegistry>) {
        self.0
            .enqueue_finalization_registry_cleanup_job_after_collection(finalization_registry);
    }
}
