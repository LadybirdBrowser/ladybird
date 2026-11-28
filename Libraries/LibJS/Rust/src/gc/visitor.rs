/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The visitor and Trace of libgc_api, which the runtime's pointer types plug into.

use core::ffi::c_void;
use core::ptr::NonNull;

use libgc_api::visitor::{CellPointer, MemberSlot, NanBoxedValue};
pub use libgc_api::visitor::{CellSlots, SlotKind, Trace, Visitor};

use crate::layout::cell::Gc;
use crate::layout::member::{Member, NullableMember};
use crate::layout::value::Value;

// SAFETY: A Gc is a non-null pointer to the start of a live cell.
unsafe impl<T: ?Sized> CellPointer for Gc<T> {
    #[inline(always)]
    fn cell_address(self) -> NonNull<c_void> {
        self.as_non_null().cast()
    }
}

// SAFETY: A member is a compressed pointer to a live cell in a u32.
unsafe impl<T> MemberSlot for Member<T> {
    #[inline(always)]
    fn slot(&self) -> *const u32 {
        core::ptr::from_ref(self).cast()
    }

    #[inline(always)]
    fn cell_address(&self) -> Option<NonNull<c_void>> {
        Some(self.get().as_non_null().cast())
    }
}

// SAFETY: A nullable member is 0 or a compressed pointer to a live cell in a u32.
unsafe impl<T> MemberSlot for NullableMember<T> {
    #[inline(always)]
    fn slot(&self) -> *const u32 {
        core::ptr::from_ref(self).cast()
    }

    #[inline(always)]
    fn cell_address(&self) -> Option<NonNull<c_void>> {
        self.get().map(|cell| cell.as_non_null().cast())
    }
}

// SAFETY: A Value is a NaN-boxed u64 as LibGC decodes it.
unsafe impl NanBoxedValue for Value {
    #[inline(always)]
    fn cell_address(&self) -> Option<NonNull<c_void>> {
        self.is_cell().then(|| self.as_cell().as_non_null().cast())
    }
}

/// The slots in the first `size` bytes of `cell`, which tracing the cell visits (see CellSlots).
pub fn record_cell_slots<T>(cell: Gc<T>, size: usize) -> CellSlots {
    // SAFETY: The cell is live and of the class whose trace this is, and stays where it is during the trace.
    unsafe {
        libgc_api::visitor::record_cell_slots(
            cell.as_non_null().cast(),
            size,
            super::class::is_in_heap_region,
            super::class::class_of(cell).trace,
        )
    }
}

// SAFETY: The header holds no cells.
unsafe impl Trace for crate::layout::cell::CellHeader {
    #[inline(always)]
    fn trace(&self, _: &mut Visitor<'_>) {}
}

// SAFETY: A Gc reaches exactly its cell.
unsafe impl<T> Trace for Gc<T> {
    #[inline(always)]
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        // NB: The Gc is visited where it is, so that a collection that moves its cell rewrites it if it is in a cell.
        //     Elsewhere, such as on the stack or in malloc memory, it keeps its cell where it is.
        visitor.visit_slot(self);
    }
}

// SAFETY: A member reaches exactly its cell.
unsafe impl<T> Trace for Member<T> {
    #[inline(always)]
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        visitor.visit_member(self);
    }
}

// SAFETY: A nullable member reaches exactly its cell, if it has one.
unsafe impl<T> Trace for NullableMember<T> {
    #[inline(always)]
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        visitor.visit_member(self);
    }
}

// SAFETY: LibGC decodes the value and visits its cell, if it has one.
unsafe impl Trace for Value {
    #[inline(always)]
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        visitor.visit_values(core::slice::from_ref(self));
    }

    #[inline(always)]
    fn trace_slice<'c>(values: &'c [Self], visitor: &mut Visitor<'c>) {
        visitor.visit_values(values);
    }
}
