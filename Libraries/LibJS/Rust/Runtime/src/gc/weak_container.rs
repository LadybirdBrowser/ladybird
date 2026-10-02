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

/// WeakContainer::remove_dead_cells(), called during every collection on each container that survived it, while the
/// cells that died are intact and before they are swept. It must not allocate.
pub type RemoveDeadCells = fn(&Object, &Vm);

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

    pub fn remove_dead_cells(&self, vm: &Vm) {
        (self.remove_dead_cells)(&self.owner, vm);
    }
}
