/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! References from the runtime's cells to the cells of an embedder's C++ types, which LibGC allocates in the same heap.

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;

use super::capi::gc_visitor_visit_cell;
use super::visitor::{Trace, Visitor};

/// A reference to a C++ GC cell, such as the implementation object a host object wraps, which keeps that cell alive
/// for as long as the cell or root holding the slot is.
#[derive(Default)]
#[repr(transparent)]
pub struct ForeignCellSlot(Cell<Option<NonNull<c_void>>>);

impl ForeignCellSlot {
    pub const fn empty() -> Self {
        Self(Cell::new(None))
    }

    pub fn get(&self) -> Option<NonNull<c_void>> {
        self.0.get()
    }

    /// The cell, or null, as the C ABI passes it.
    pub fn as_ptr(&self) -> *mut c_void {
        self.get().map_or(core::ptr::null_mut(), NonNull::as_ptr)
    }

    /// # Safety
    ///
    /// `cell` must be a live cell of the heap the runtime allocates from, C++ or Rust, which the slot then keeps alive.
    pub unsafe fn set(&self, cell: Option<NonNull<c_void>>) {
        self.0.set(cell);
    }
}

// SAFETY: Visits the one cell the slot holds.
unsafe impl Trace for ForeignCellSlot {
    fn trace(&self, visitor: &mut Visitor) {
        if let Some(cell) = self.get() {
            // SAFETY: The visitor is live, and set() only stores cells of its heap, which the slot has kept alive.
            unsafe { gc_visitor_visit_cell(visitor.as_raw(), cell.as_ptr()) };
        }
    }
}
