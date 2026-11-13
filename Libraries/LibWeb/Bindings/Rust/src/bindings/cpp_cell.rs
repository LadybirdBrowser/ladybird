/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{ForeignCellSlot, Gc, GcWeak, Trace, Vm};

/// A cell of LibWeb's C++ code, which this keeps alive.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppCell(ForeignCellSlot);

impl Clone for CppCell {
    fn clone(&self) -> Self {
        // SAFETY: The slot keeps the cell alive.
        unsafe { Self::from_raw(self.0.as_ptr()) }
    }
}

impl CppCell {
    /// # Safety
    ///
    /// `cell` must be a live cell of LibWeb's C++ code.
    pub(crate) unsafe fn from_raw(cell: *mut c_void) -> Self {
        let slot = ForeignCellSlot::empty();
        // SAFETY: The caller passes a live cell.
        unsafe { slot.set(Some(NonNull::new(cell).expect("C++ passes a cell"))) };
        Self(slot)
    }

    pub(crate) fn as_ptr(&self) -> *mut c_void {
        self.0.as_ptr()
    }
}

/// A cell of LibWeb's C++ code, which this does not keep alive.
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct WeakCppCell(GcWeak<c_void>);

impl WeakCppCell {
    pub(crate) fn new(vm: &Vm, cell: &CppCell) -> Self {
        let cell = NonNull::new(cell.as_ptr()).expect("a CppCell holds a cell");
        // SAFETY: The CppCell keeps the cell alive.
        Self(GcWeak::new(vm.heap(), unsafe { Gc::from_non_null(cell) }))
    }

    pub(crate) fn get(&self) -> Option<CppCell> {
        // SAFETY: A cell that the weak reference still has is live.
        self.0.get().map(|cell| unsafe { CppCell::from_raw(cell.as_ptr()) })
    }
}
