/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! GC::WeakContainer: a cell that holds other cells without keeping them alive, and forgets them once they die. The
//! VM keeps a list of them, which its sweep callback prunes the way LibGC prunes its weak containers.

use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::runtime::finalization_registry::FinalizationRegistry;

/// WeakContainer::remove_dead_cells(), called during every collection on each container that survived it, while the
/// cells that died are intact and before they are swept. It gets no VM, as it must not allocate.
pub type RemoveDeadCells = fn(&Object, &Sweep<'_>);

/// What a weak container can do while it removes its dead cells during a collection.
pub struct Sweep<'vm>(&'vm Vm);

impl Sweep<'_> {
    /// Hands `finalization_registry` to HostEnqueueFinalizationRegistryCleanupJob once the collection in progress is
    /// over.
    pub fn enqueue_finalization_registry_cleanup_job(&self, finalization_registry: Gc<FinalizationRegistry>) {
        self.0
            .enqueue_finalization_registry_cleanup_job_after_collection(finalization_registry);
    }
}

pub struct WeakContainer {
    owner: Gc<Object>,
    remove_dead_cells: RemoveDeadCells,
}

impl WeakContainer {
    pub fn new(owner: Gc<Object>, remove_dead_cells: RemoveDeadCells) -> Self {
        Self {
            owner,
            remove_dead_cells,
        }
    }

    pub fn owner(&self) -> Gc<Object> {
        self.owner
    }

    pub(crate) fn remove_dead_cells(&self, vm: &Vm) {
        (self.remove_dead_cells)(&self.owner, &Sweep(vm));
    }
}
