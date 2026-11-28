/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The cells a document keeps weakly, such as its live ranges and its NodeIterators, as Rust reads them in place.
//!
//! C++ keeps each set as a DOM::WeakCellRegistry: a vector of GC::Weak pointers, which the document owns, and whose
//! entries and size it mirrors into a WeakCellSpan of the document's core whenever it changes them. The vector is no
//! root, so a cell in it dies as soon as nothing else keeps it alive, but the collector knows of each of its weak
//! pointers: it points one at the new address of a cell it moves, and nulls one whose cell dies. After every collection,
//! the registry drops the null entries, see WeakCellRegistry::remove_dead_cells().
//!
//! Rust only reads the entries, and only while nothing calls into C++, which could change the vector, or collect.

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;

/// Where GC::WeakImpl keeps the address of its cell, which WeakCellRegistry.h asserts.
pub const WEAK_IMPL_VALUE_OFFSET: usize = 16;

/// The entries of a DOM::WeakCellRegistry, as C++ mirrors them: a pointer to its first GC::Weak, each of which is a
/// pointer to the GC::WeakImpl that holds the address of its cell, or null once the cell is dead, and how many there are.
#[repr(C)]
pub struct WeakCellSpan {
    entries: Cell<*const *const c_void>,
    length: Cell<usize>,
}

impl WeakCellSpan {
    #[inline(always)]
    pub fn len(&self) -> usize {
        self.length.get()
    }

    #[inline(always)]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// The start of the cell of the entry at `index`, or none if the cell is dead.
    ///
    /// # Safety
    ///
    /// `index` must be less than the length, and nothing may have changed the registry since C++ last mirrored it.
    #[inline(always)]
    pub unsafe fn cell_at(&self, index: usize) -> Option<NonNull<u8>> {
        debug_assert!(index < self.len());
        // SAFETY: Guaranteed by the caller: the entry is a live GC::Weak, which points at a GC::WeakImpl.
        unsafe {
            let weak_impl = *self.entries.get().add(index);
            let cell = *weak_impl.byte_add(WEAK_IMPL_VALUE_OFFSET).cast::<*mut u8>();
            NonNull::new(cell)
        }
    }

    /// Calls `callback` with the start of the cell of each live entry, in the registry's order.
    ///
    /// # Safety
    ///
    /// Nothing may have changed the registry since C++ last mirrored it, and `callback` must not change it, so must not
    /// call into C++.
    #[inline(always)]
    pub unsafe fn for_each_cell(&self, mut callback: impl FnMut(NonNull<u8>)) {
        for index in 0..self.len() {
            // SAFETY: Guaranteed by the caller.
            if let Some(cell) = unsafe { self.cell_at(index) } {
                callback(cell);
            }
        }
    }
}

#[cfg(test)]
impl WeakCellSpan {
    pub(crate) fn empty_for_test() -> Self {
        Self {
            entries: Cell::new(core::ptr::null()),
            length: Cell::new(0),
        }
    }

    /// Mirrors `entries`, which must outlive the span's use, as C++ does.
    pub(crate) fn set_for_test(&self, entries: &[*const c_void]) {
        self.entries.set(entries.as_ptr());
        self.length.set(entries.len());
    }
}

/// A GC::WeakImpl for tests, which C++ allocates in a GC::WeakBlock.
#[cfg(test)]
#[repr(C)]
pub(crate) struct WeakImplForTest {
    _reference_count: usize,
    _state: u32,
    value: *mut u8,
}

#[cfg(test)]
impl WeakImplForTest {
    /// A weak pointer to the cell at `value`, or a cleared one if it is null.
    pub(crate) fn new(value: *mut u8) -> Self {
        Self {
            _reference_count: 1,
            _state: 0,
            value,
        }
    }
}

#[cfg(test)]
const _: () = assert!(core::mem::offset_of!(WeakImplForTest, value) == WEAK_IMPL_VALUE_OFFSET);

// NB: WeakCellRegistry.h asserts the same layout of the struct it includes.
const _: () = assert!(size_of::<WeakCellSpan>() == 16 && align_of::<WeakCellSpan>() == 8);
const _: () = assert!(core::mem::offset_of!(WeakCellSpan, entries) == 0);
const _: () = assert!(core::mem::offset_of!(WeakCellSpan, length) == 8);
